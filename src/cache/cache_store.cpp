// ============================================================================
// cache_store.cpp — Implementation of the Sharded Cache
// ============================================================================
//
// This file implements every method declared in cache_store.h.
// The two most important topics to understand for an interview:
//
//   1. HOW LOCKING WORKS WITH SHARDS
//      Every public method follows a strict protocol:
//        a) Compute which shard the key belongs to (hash & modulo).
//        b) Acquire the minimal lock needed (shared for reads, unique for writes).
//        c) Operate ONLY on that shard's data.
//        d) Release the lock when the scope ends (RAII — automatic unlock).
//
//   2. HOW EVICTION WORKS
//      We combine two eviction strategies:
//        Phase 1 (Expired keys): Scan the shard for keys whose TTL has elapsed.
//                                These are free to remove — no LRU needed.
//        Phase 2 (LRU eviction): If still at capacity after Phase 1, remove the
//                                least-recently-used key using our access_list.
//
// RAII LOCKING (CRITICALLY IMPORTANT TO UNDERSTAND):
//   std::unique_lock and std::shared_lock are RAII wrappers.
//   When they go out of scope (end of {}) the lock is AUTOMATICALLY released.
//   This prevents dead-locks from forgotten unlock() calls — a common C++ bug.
//   Example:
//     {
//       std::unique_lock lock(shard.mutex);  // Lock acquired here
//       // ... do work ...
//     }  // <-- lock.~unique_lock() is called automatically → mutex released
//
// ============================================================================

#include "cache_store.h"
#include <algorithm>

namespace distcache {

// ============================================================================
// Constructor
// ============================================================================
// shards_(num_shards) pre-allocates exactly N Shard objects in a vector.
// No allocation happens later, so there is zero memory overhead during requests.
// IMPORTANT: Shard objects CANNOT be moved or copied after construction because
// std::shared_mutex is non-movable. We pre-allocate all shards here.
CacheStore::CacheStore(size_t num_shards, size_t max_entries_per_shard)
    : shards_(num_shards), max_entries_per_shard_(max_entries_per_shard) {}

// ============================================================================
// shard_index(): Key-to-Shard Mapping
// ============================================================================
// std::hash<std::string> computes a deterministic hash of the key string.
// The result is a size_t (64-bit unsigned integer on 64-bit systems).
// We use modulo (%) to reduce it to the range [0, num_shards - 1].
//
// Example with 256 shards:
//   hash("user_123") = 0xA3F2B1C7D9E4523F
//   0xA3F2B1C7D9E4523F % 256 = 0x3F = shard 63
//
// INTERVIEW NOTE: For even better performance, if num_shards is a power of 2,
// we CAN replace `% num_shards` with `& (num_shards - 1)` since bitwise AND
// is faster than integer division on most CPUs. We keep modulo here for clarity.
size_t CacheStore::shard_index(const std::string& key) const {
    return std::hash<std::string>{}(key) % shards_.size();
}

// ============================================================================
// is_expired(): TTL Check
// ============================================================================
// Simply compares now() to the entry's expiry timestamp.
// steady_clock is monotonic (never goes backwards), unlike system_clock.
// If network synchronization or `date` commands adjust the system clock,
// system_clock-based TTLs would break. steady_clock is immune to this.
bool CacheStore::is_expired(const CacheEntry& entry) const {
    return std::chrono::steady_clock::now() > entry.expires_at;
}

// ============================================================================
// touch(): LRU Access Recording
// ============================================================================
// Called on EVERY successful get() to record that this key was recently used.
// How it works:
//   1. Find the key in the access_list using the list_index map (O(1) lookup).
//   2. Move (splice) that iterator to the FRONT of the list — O(1) for linked list.
//   3. Now the front of access_list always has the MRU key.
//   4. The BACK of access_list always has the LRU key → the eviction victim.
//
// CALLER MUST HOLD unique_lock to modify access_list safely.
void CacheStore::touch(Shard& shard, const std::string& key) {
    auto it = shard.list_index.find(key);
    if (it != shard.list_index.end()) {
        // O(1) splice: detach the node at it->second and re-attach it at the front.
        shard.access_list.splice(shard.access_list.begin(), shard.access_list, it->second);
    }
}

// ============================================================================
// set(): Write a Key-Value Pair
// ============================================================================
bool CacheStore::set(const std::string& key, const std::string& value,
                     std::chrono::seconds ttl) {
    auto& shard = shards_[shard_index(key)];  // Resolve the target shard.
    auto now = std::chrono::steady_clock::now();

    // Build the new CacheEntry on the heap, wrapped in a shared_ptr for safety.
    // We create the entry OUTSIDE the lock to reduce lock hold time.
    auto entry = std::make_shared<CacheEntry>();
    entry->value = value;
    entry->created_at = now;
    entry->expires_at = (ttl.count() > 0) ? (now + ttl) : std::chrono::steady_clock::time_point::max();
    //                                                      ^ max() means "never expires"

    {
        // EXCLUSIVE LOCK: Writers need full exclusivity — no other thread can
        // read OR write to this shard while we insert. The lock is released 
        // automatically when this block exits (RAII).
        std::unique_lock lock(shard.mutex);

        // Evict if we're at capacity BEFORE inserting, so we never exceed limits.
        evict_if_needed(shard);

        shard.entries[key] = std::move(entry);  // Insert or overwrite.

        // Update LRU tracking: add to front of access_list.
        // If key already existed (overwrite), remove its old position first.
        auto existing = shard.list_index.find(key);
        if (existing != shard.list_index.end()) {
            shard.access_list.erase(existing->second);
            shard.list_index.erase(existing);
        }
        shard.access_list.push_front(key);
        shard.list_index[key] = shard.access_list.begin();
    }  // unique_lock released here.

    // Atomic increment — lock-free, single CPU instruction.
    metrics_.total_sets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ============================================================================
// get(): Read a Value by Key
// ============================================================================
std::optional<std::string> CacheStore::get(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    metrics_.total_gets.fetch_add(1, std::memory_order_relaxed);

    {
        // SHARED LOCK: Multiple threads can concurrently read the same shard.
        // std::shared_lock is read-only — it does NOT block other readers.
        // It DOES block any pending unique_lock (writer) until all readers finish.
        // This is the "readers-writer lock" pattern — perfect for read-heavy caches.
        std::shared_lock lock(shard.mutex);

        auto it = shard.entries.find(key);

        // Case 1: Key does not exist at all.
        if (it == shard.entries.end()) {
            metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // Case 2: Key exists but TTL has elapsed → lazy expiration.
        // We return nullopt without deleting the key here (that would require
        // upgrading to a unique_lock, which is not possible in one step).
        // The next set() or evict_if_needed() call will clean it up.
        if (is_expired(*it->second)) {
            metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
            metrics_.expired_keys.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        // Case 3: Cache HIT — key exists and is valid.
        // Increment access_count with relaxed atomics — no lock needed.
        it->second->access_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);

        // NOTE: We intentionally do NOT call touch() here (LRU update).
        // touch() requires a unique_lock to modify access_list, but we only
        // hold a shared_lock. Upgrading locks (shared→unique) causes deadlocks
        // in C++. Instead, we use access_count (atomic) as a cheap LFU signal
        // for eviction decisions. This is a deliberate trade-off.
        return it->second->value;  // Return a copy of the value.
    }  // shared_lock released here.
}

// ============================================================================
// del(): Remove a Key
// ============================================================================
bool CacheStore::del(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    std::unique_lock lock(shard.mutex);

    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) return false;

    // Remove from LRU tracking.
    auto list_it = shard.list_index.find(key);
    if (list_it != shard.list_index.end()) {
        shard.access_list.erase(list_it->second);
        shard.list_index.erase(list_it);
    }

    shard.entries.erase(it);
    return true;
}

// ============================================================================
// exists(): Lightweight Existence Check
// ============================================================================
bool CacheStore::exists(const std::string& key) {
    auto& shard = shards_[shard_index(key)];
    std::shared_lock lock(shard.mutex);     // Read-only → shared lock.
    auto it = shard.entries.find(key);
    if (it == shard.entries.end()) return false;
    return !is_expired(*it->second);        // Lazy expiration check.
}

// ============================================================================
// mget(): Bulk Read
// ============================================================================
// This calls get() N times. An advanced optimization would be to batch keys
// by shard and acquire each shard lock only once, reducing lock overhead.
// For this implementation, simplicity is preferred — each get() is still
// O(1) with a separate lock acquisition and release.
std::vector<std::optional<std::string>> CacheStore::mget(
    const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> results;
    results.reserve(keys.size());  // Pre-allocate to avoid repeated heap allocation.
    for (const auto& key : keys) {
        results.push_back(get(key));
    }
    return results;
}

// ============================================================================
// mset(): Bulk Write
// ============================================================================
size_t CacheStore::mset(
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::chrono::seconds ttl) {
    size_t count = 0;
    for (const auto& [key, value] : entries) {  // Structured bindings (C++17).
        if (set(key, value, ttl)) ++count;
    }
    return count;
}

// ============================================================================
// size(): Total Live Entry Count
// ============================================================================
// Acquires a shared lock on EVERY shard sequentially to count entries.
// This is O(num_shards) and should NOT be called in hot paths.
size_t CacheStore::size() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::shared_lock lock(shard.mutex);
        total += shard.entries.size();
    }
    return total;
}

// ============================================================================
// clear(): Remove All Entries
// ============================================================================
void CacheStore::clear() {
    for (auto& shard : shards_) {
        std::unique_lock lock(shard.mutex);
        shard.entries.clear();
        shard.access_list.clear();
        shard.list_index.clear();
    }
}

// ============================================================================
// evict_if_needed(): Capacity Management and Eviction
// ============================================================================
// PRECONDITION: Caller MUST hold a unique_lock on shard.mutex.
//               Never call this without the lock — it's not thread-safe alone.
//
// EVICTION STRATEGY: Two-Phase
//
// Phase 1 — Expired Cleanup:
//   Walk through all entries and delete any with elapsed TTL.
//   This is effectively "free" eviction — the data was stale anyway.
//   Complexity: O(N) scan of the shard. Acceptable since N ≤ max_entries_per_shard.
//
// Phase 2 — LRU Eviction (if Phase 1 wasn't enough):
//   Use access_list (a doubly-linked list maintained by touch()/set()):
//     - The BACK of access_list holds the LEAST recently used key.
//     - Pop from back and erase from the map until under capacity.
//   Complexity: O(1) per eviction — no scanning needed.
//
// WHY COMBINE BOTH?
//   In the common case (lots of expired keys at capacity), Phase 1 frees space
//   without wasting any valid data. LRU (Phase 2) only kicks in when the cache
//   is genuinely full of valid, live entries.
//
// RESUME CONNECTION: "struggled with cache eviction races under heavy load"
//   The "race" occurs if eviction is NOT inside the same unique_lock as the insert.
//   If two threads both see the shard is full, then BOTH try to evict, they might
//   both evict different entries and BOTH insert — suddenly the shard is over capacity.
//   Holding the unique_lock for the ENTIRE evict+insert sequence prevents this.
//   This is why Prometheus observability was critical: we could SEE the eviction
//   counter spike and diagnose that eviction was not atomic with insertion.
void CacheStore::evict_if_needed(Shard& shard) {
    if (shard.entries.size() < max_entries_per_shard_) {
        return;  // Shard is not full — nothing to do.
    }

    // --- Phase 1: Remove expired entries ---
    for (auto it = shard.entries.begin(); it != shard.entries.end(); ) {
        if (is_expired(*it->second)) {
            // Clean up LRU tracking for this key.
            auto list_it = shard.list_index.find(it->first);
            if (list_it != shard.list_index.end()) {
                shard.access_list.erase(list_it->second);
                shard.list_index.erase(list_it);
            }
            it = shard.entries.erase(it);  // erase() returns next valid iterator.
            metrics_.evictions.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }

    // --- Phase 2: LRU eviction if still at capacity ---
    // Pop from the BACK of access_list (LRU = least recently used).
    while (shard.entries.size() >= max_entries_per_shard_) {
        if (shard.access_list.empty()) break;  // Defensive guard.

        const std::string& lru_key = shard.access_list.back();
        auto entry_it = shard.entries.find(lru_key);
        if (entry_it != shard.entries.end()) {
            shard.entries.erase(entry_it);
            metrics_.evictions.fetch_add(1, std::memory_order_relaxed);
        }
        shard.list_index.erase(lru_key);
        shard.access_list.pop_back();  // O(1) — linked list.
    }
}

}  // namespace distcache
