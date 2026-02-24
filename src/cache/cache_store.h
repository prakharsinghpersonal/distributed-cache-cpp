#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace distcache {

/**
 * Thread-safe, high-frequency distributed cache store.
 * Uses sharded buckets with fine-grained locking to approach
 * lock-free performance at 120K+ concurrent RPS.
 *
 * Design:
 *   - Key space is partitioned across N shards (default 256)
 *   - Each shard has its own shared_mutex for read-heavy workloads
 *   - LRU eviction within each shard keeps memory bounded
 */

struct CacheEntry {
    std::string value;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point expires_at;
    std::atomic<uint64_t> access_count{0};
};

struct CacheMetrics {
    std::atomic<uint64_t> total_gets{0};
    std::atomic<uint64_t> total_sets{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> expired_keys{0};

    double hit_rate() const {
        auto total = total_gets.load();
        return total > 0 ? static_cast<double>(cache_hits.load()) / total : 0.0;
    }
};

class CacheStore {
public:
    explicit CacheStore(size_t num_shards = 256, size_t max_entries_per_shard = 10000);
    ~CacheStore() = default;

    // Core operations
    bool set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds(300));
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);

    // Bulk operations
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);
    size_t mset(const std::vector<std::pair<std::string, std::string>>& entries,
                std::chrono::seconds ttl = std::chrono::seconds(300));

    // Metrics
    CacheMetrics& metrics() { return metrics_; }
    size_t size() const;
    void clear();

private:
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<CacheEntry>> entries;
    };

    size_t shard_index(const std::string& key) const;
    void evict_if_needed(Shard& shard);
    bool is_expired(const CacheEntry& entry) const;

    std::vector<Shard> shards_;
    size_t max_entries_per_shard_;
    CacheMetrics metrics_;
};

} // namespace distcache
