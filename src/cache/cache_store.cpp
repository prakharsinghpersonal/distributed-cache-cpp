#include "cache_store.h"

#include <algorithm>

namespace distcache {

CacheStore::CacheStore(size_t num_shards, size_t max_entries_per_shard)
    : shards_(num_shards), max_entries_per_shard_(max_entries_per_shard) {}

size_t CacheStore::shard_index(const std::string& key) const {
    return std::hash<std::string>{}(key) % shards_.size();
}

bool CacheStore::is_expired(const CacheEntry& entry) const {
    return std::chrono::steady_clock::now() > entry.expires_at;
}

bool CacheStore::set(const std::string& key, const std::string& value,
                     std::chrono::seconds ttl) {
    auto& shard = shards_[shard_index(key)];
    auto now = std::chrono::steady_clock::now();

    auto entry = std::make_shared<CacheEntry>();
    entry->value = value;
    entry->created_at = now;
    entry->expires_at = now + ttl;

    {
        std::unique_lock lock(shard.mutex);
        evict_if_needed(shard);
        shard.entries[key] = std::move(entry);
    }

    metrics_.total_sets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::optional<std::string> CacheStore::get(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    metrics_.total_gets.fetch_add(1, std::memory_order_relaxed);

    {
        std::shared_lock lock(shard.mutex);
        auto it = shard.entries.find(key);
        if (it == shard.entries.end()) {
            metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        if (is_expired(*it->second)) {
            metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
            metrics_.expired_keys.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;  // Lazy expiration
        }

        it->second->access_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
        return it->second->value;
    }
}

bool CacheStore::del(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    std::unique_lock lock(shard.mutex);
    return shard.entries.erase(key) > 0;
}

bool CacheStore::exists(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) return false;
    return !is_expired(*it->second);
}

std::vector<std::optional<std::string>> CacheStore::mget(
    const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> results;
    results.reserve(keys.size());
    for (const auto& key : keys) {
        results.push_back(get(key));
    }
    return results;
}

size_t CacheStore::mset(
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::chrono::seconds ttl) {
    size_t count = 0;
    for (const auto& [key, value] : entries) {
        if (set(key, value, ttl)) ++count;
    }
    return count;
}

size_t CacheStore::size() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard.mutex);
        total += shard.entries.size();
    }
    return total;
}

void CacheStore::clear() {
    for (auto& shard : shards_) {
        std::unique_lock lock(shard.mutex);
        shard.entries.clear();
    }
}

void CacheStore::evict_if_needed(Shard& shard) {
    // Caller must hold unique lock on shard.mutex
    if (shard.entries.size() < max_entries_per_shard_) return;

    // Evict expired entries first
    for (auto it = shard.entries.begin(); it != shard.entries.end();) {
        if (is_expired(*it->second)) {
            it = shard.entries.erase(it);
            metrics_.evictions.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }

    // If still over capacity, evict least accessed
    while (shard.entries.size() >= max_entries_per_shard_) {
        auto victim = std::min_element(
            shard.entries.begin(), shard.entries.end(),
            [](const auto& a, const auto& b) {
                return a.second->access_count.load() < b.second->access_count.load();
            });
        if (victim != shard.entries.end()) {
            shard.entries.erase(victim);
            metrics_.evictions.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace distcache
