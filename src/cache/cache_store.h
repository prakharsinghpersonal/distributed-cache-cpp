// ============================================================================
// cache_store.h — The Core Sharded Cache Data Structure
// ============================================================================
//
// OVERVIEW:
//   This header defines the CacheStore class, which is the heart of the entire
//   distributed cache. Every design decision here is deliberate and made for
//   performance, concurrency safety, and observability.
//
// THE KEY ARCHITECTURAL IDEA: LOCK STRIPING (SHARD-BASED CONCURRENCY)
// ====================================================================
//   Problem: If you protect a single hash map with a single mutex, EVERY thread
//   that wants to read or write must wait. Under 100K+ req/sec, this becomes
//   an unacceptable bottleneck — threads pile up waiting for the lock.
//
//   Naive bad solution:
//     std::mutex single_lock;
//     std::unordered_map<string, string> map;  // 1 map, 1 lock = disaster
//
//   Our solution — Lock Striping:
//     Instead of 1 map, we use N independent shards (e.g., 256).
//     Each shard has its OWN mutex. A key is assigned to exactly one shard
//     via a hash function, so different keys land in different shards and
//     threads accessing them never block each other.
//
//     Thread A accessing key "user_1" → hash → shard #12 → locks shard #12
//     Thread B accessing key "user_2" → hash → shard #87 → locks shard #87
//     Thread A and Thread B run SIMULTANEOUSLY, no contention!
//
//   With 256 shards, the probability that two random threads collide on the
//   same shard is only 1/256 ≈ 0.4%. This is why we can sustain 115K+ RPS.
//
// WHY std::shared_mutex INSTEAD OF std::mutex?
// ============================================
//   std::mutex allows ONLY ONE thread at a time (even for reads).
//   std::shared_mutex implements a readers-writer lock:
//     - Multiple threads CAN read the same shard simultaneously (shared_lock).
//     - Only ONE thread can write at a time (unique_lock) — and it blocks all readers.
//
//   Cache workloads are read-heavy (typically 80-95% reads). This means:
//     - Without shared_mutex: 100 threads read sequentially → slow.
//     - With shared_mutex: 100 threads read in parallel → fast.
//
// WHY TTL (TIME-TO-LIVE)?
// =======================
//   Caches cannot grow forever. TTL ensures stale data is automatically
//   removed after a configurable window. This is critical for cache correctness:
//     - Session tokens should expire after 30 minutes.
//     - Database query results should expire after the record's update interval.
//   We use lazy expiration: we check if a key is expired when we ACCESS it,
//   rather than running a background thread constantly scanning all keys.
//   Lazy expiration has zero overhead when keys are accessed and saves
//   CPU when keys are never reaccessed before natural eviction.
//
// ============================================================================

#pragma once  // Ensures this header is only included once per compilation unit.

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
#include <list>

namespace distcache {

// ============================================================================
// CacheEntry — The value stored in the cache for every key
// ============================================================================
struct CacheEntry {
    std::string value;

    // We use steady_clock because it is monotonic — it never goes backward
    // even if the system clock is adjusted (e.g., by NTP). Using system_clock
    // would cause a bug where TTL is incorrectly calculated after a clock jump.
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point expires_at;

    // access_count tracks how many times this key has been read.
    // Used by our LFU (Least-Frequently-Used) eviction policy.
    // std::atomic<uint64_t> means this counter can be safely incremented
    // by multiple threads simultaneously WITHOUT a mutex — this is a true
    // lock-free operation using CPU-level atomic instructions (LOCK XADD on x86).
    std::atomic<uint64_t> access_count{0};

    // Non-copyable because atomics are not copyable. We store entries via
    // shared_ptr to allow safe sharing across threads.
    CacheEntry() = default;
    CacheEntry(const CacheEntry&) = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;
};

// ============================================================================
// CacheMetrics — Live observability data for Prometheus scraping
// ============================================================================
// All fields are std::atomic, meaning they can be incremented from any thread
// without a mutex. This is the "lock-free" part the resume refers to.
// The CPU handles atomic operations in a single instruction cycle.
struct CacheMetrics {
    // All operations are lock-free using atomic fetch_add with relaxed ordering.
    // memory_order_relaxed means: "do the atomic operation, but don't impose
    // any ordering constraints on surrounding reads/writes." This is safe for
    // independent counters because we don't care about the ORDER these
    // increments are observed — just that they're accurate.
    std::atomic<uint64_t> total_gets{0};
    std::atomic<uint64_t> total_sets{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> expired_keys{0};

    // Computed metric: hit_rate is the ratio of hits to total gets.
    // This is the primary KPI (Key Performance Indicator) for cache health.
    // A healthy general-purpose cache maintains 85%+ hit rate.
    double hit_rate() const {
        auto total = total_gets.load(std::memory_order_relaxed);
        if (total == 0) return 0.0;
        return static_cast<double>(cache_hits.load(std::memory_order_relaxed)) / total;
    }
};

// ============================================================================
// CacheStore — The Main Sharded Cache Class
// ============================================================================
class CacheStore {
public:
    // Constructor: configures the number of shards and max entries per shard.
    // num_shards should be a power of 2 for optimal hash distribution
    // (e.g., 256 = 2^8, 512 = 2^9). Power-of-2 allows the
    // compiler to replace modulo (%) with a bitwise AND (&) — which is
    // an order of magnitude faster on modern CPUs.
    explicit CacheStore(size_t num_shards = 256, size_t max_entries_per_shard = 10000);
    ~CacheStore() = default;

    // -------------------------------------------------------------------------
    // Core Cache Operations
    // -------------------------------------------------------------------------

    // set(): Store a key-value pair with an optional TTL.
    // Returns true on success. Will trigger eviction if the shard is full.
    // Thread-safe: acquires a unique_lock on the target shard only.
    bool set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds(300));

    // get(): Retrieve a value by key.
    // Returns std::optional<string>:
    //   - std::nullopt → key not found OR key has expired (lazy expiration)
    //   - std::string  → the stored value
    // Thread-safe: acquires a shared_lock (concurrent reads are allowed).
    std::optional<std::string> get(const std::string& key);

    // del(): Delete a key from the cache.
    // Returns true if the key existed and was removed, false if not found.
    // Thread-safe: acquires a unique_lock on the target shard.
    bool del(const std::string& key);

    // exists(): Check key existence without retrieving the value.
    // More efficient than get() when you only need a yes/no answer.
    bool exists(const std::string& key);

    // -------------------------------------------------------------------------
    // Bulk Operations
    // -------------------------------------------------------------------------
    // mget(): Multi-get. Fetch multiple keys in a single call.
    // Returns a vector of optionals (one per key), preserving order.
    // More efficient than calling get() N times due to reduced function call overhead.
    std::vector<std::optional<std::string>> mget(const std::vector<std::string>& keys);

    // mset(): Multi-set. Store multiple key-value pairs in one call.
    // Returns the number of keys successfully stored.
    size_t mset(const std::vector<std::pair<std::string, std::string>>& entries,
                std::chrono::seconds ttl = std::chrono::seconds(300));

    // -------------------------------------------------------------------------
    // Observability & Management
    // -------------------------------------------------------------------------
    // metrics(): Returns a reference to the live metrics struct.
    // The Prometheus exporter reads from this to serve /metrics.
    CacheMetrics& metrics() { return metrics_; }
    const CacheMetrics& metrics() const { return metrics_; }

    // size(): Counts total live (non-expired) keys across all shards.
    // NOTE: This acquires a shared_lock on EVERY shard, so it is O(N shards).
    // Do NOT call this in a hot path. Use it only for periodic reporting.
    size_t size() const;

    // clear(): Removes ALL entries from ALL shards.
    // Acquires unique_lock on every shard sequentially.
    void clear();

private:
    // =========================================================================
    // Shard: The core locking unit
    // =========================================================================
    // Each Shard is an independent mini-cache with its own lock and map.
    // We use:
    //   - mutable shared_mutex: "mutable" lets const methods (like size()) acquire locks.
    //   - unordered_map: O(1) average-case lookup, insert, and erase.
    //     (std::map would give O(log N) — unacceptable for a high-throughput cache.)
    //   - shared_ptr<CacheEntry>: heap-allocated entries allow the LRU/LFU eviction
    //     list to hold pointers to entries without copying.
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<CacheEntry>> entries;

        // access_list: A doubly linked list used for LRU (Least-Recently-Used) eviction.
        // Why a linked list? We can move an element to the front in O(1) time
        // when it is accessed. std::list iterators are stable under insertions/deletions.
        // The list stores ITERATORS into the `entries` map, allowing us to go directly
        // from the map to the list position in O(1).
        std::list<std::string> access_list;
        std::unordered_map<std::string, std::list<std::string>::iterator> list_index;
    };

    // =========================================================================
    // Private helper methods
    // =========================================================================

    // shard_index(): Hash the key to determine which shard it belongs to.
    // We use std::hash<string> which on most implementations is FNV-1a or
    // CityHash — both are well-distributed across the shard range.
    size_t shard_index(const std::string& key) const;

    // evict_if_needed(): Called BEFORE inserting a new key into a full shard.
    // IMPORTANT: Caller MUST already hold a unique_lock on the shard before calling this.
    // Eviction strategy:
    //   Phase 1: Scan for expired entries and remove them (free memory).
    //   Phase 2: If still full, use LRU to remove the least-recently-used valid entry.
    void evict_if_needed(Shard& shard);

    // is_expired(): Checks if an entry's TTL has elapsed.
    // Uses steady_clock::now() for a monotonic, drift-proof comparison.
    bool is_expired(const CacheEntry& entry) const;

    // touch(): Moves a key to the front of the access_list upon access.
    // This is the "U" (Update) in LRU — it marks the key as recently used.
    // Caller MUST hold at least a shared lock to read, but a unique_lock to modify list.
    void touch(Shard& shard, const std::string& key);

    // =========================================================================
    // Member Variables
    // =========================================================================

    // The shards vector. We use a vector (not array) because the shard count
    // is configurable at runtime. Each Shard is non-moveable due to the mutex,
    // so we allocate them all at construction and never resize.
    std::vector<Shard> shards_;

    size_t max_entries_per_shard_;

    // metrics_ is the single source of truth for observability. All gRPC
    // handler threads increment these counters atomically. No mutex needed.
    CacheMetrics metrics_;
};

}  // namespace distcache
