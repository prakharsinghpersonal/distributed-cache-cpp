// ============================================================================
// cache_service.cpp — gRPC Handler Implementations
// ============================================================================
//
// This file implements the actual network request handling logic.
// When a client sends a gRPC request over the network, gRPC deserializes it,
// picks a thread from its internal completion queue thread pool, and calls
// one of these methods. Our job is to:
//   1. Validate the request (check for empty keys, etc.).
//   2. Call the appropriate CacheStore method.
//   3. Populate the response proto message.
//   4. Increment the Prometheus observability counters.
//   5. Return grpc::Status::OK.
//
// THREAD SAFETY NOTE:
//   Multiple threads can call these handlers SIMULTANEOUSLY (gRPC uses a thread pool).
//   We do NOT need any mutexes here because:
//     - CacheStore methods are thread-safe (internal sharded locking).
//     - MetricsExporter uses atomic counters (lock-free increments).
//     - We do not share any mutable state between handler calls.
//   This is intentional — keeping handlers stateless maximizes parallelism.
//
// GRPC STATUS CODES:
//   grpc::Status::OK           → Everything worked. HTTP 200 equivalent.
//   grpc::INVALID_ARGUMENT     → Bad request (empty key, etc.). HTTP 400 equivalent.
//   grpc::NOT_FOUND            → Resource does not exist. HTTP 404 equivalent.
//   grpc::INTERNAL             → Server error. HTTP 500 equivalent.
//   Returning the correct status code is important for clients to handle errors
//   gracefully (retry, fallback, alert, etc.).
//
// ============================================================================

#include "cache_service.h"
#include <chrono>

namespace distcache {

// CacheServiceImpl receives the CacheStore and MetricsExporter via Dependency Injection.
// shared_ptr reference counts are incremented here, ensuring these objects stay alive
// as long as the service exists.
CacheServiceImpl::CacheServiceImpl(std::shared_ptr<CacheStore> store,
                                    std::shared_ptr<MetricsExporter> metrics)
    : store_(std::move(store)), metrics_(std::move(metrics)) {}

// ============================================================================
// Set Handler
// ============================================================================
// This is the most common write operation. A client sends:
//   SetRequest { key: "session:user_42", value: "<json blob>", ttl_seconds: 1800 }
// We store it and respond: SetResponse { success: true }
grpc::Status CacheServiceImpl::Set(grpc::ServerContext* context,
                                   const SetRequest* request,
                                   SetResponse* response) {
    // Input validation: an empty key would corrupt the shard index calculation.
    // Returning INVALID_ARGUMENT lets clients know they made a bad request,
    // not that the server crashed.
    if (request->key().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
    }

    // Convert the TTL from int32 seconds (proto type) to std::chrono::seconds (C++ type).
    // TTL of 0 means "no expiration" — we use seconds(0) as the sentinel value,
    // and CacheStore::set() treats it as max time_point.
    auto ttl = std::chrono::seconds(request->ttl_seconds());

    bool success = store_->set(request->key(), request->value(), ttl);
    response->set_success(success);

    // Record the set in Prometheus — this counter will appear in /metrics output.
    if (metrics_) metrics_->increment_sets();

    return grpc::Status::OK;
}

// ============================================================================
// Get Handler
// ============================================================================
// Client sends: GetRequest { key: "session:user_42" }
// We respond: GetResponse { found: true, value: "<json blob>" }
//         or: GetResponse { found: false, value: "" }
grpc::Status CacheServiceImpl::Get(grpc::ServerContext* context,
                                   const GetRequest* request,
                                   GetResponse* response) {
    if (request->key().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
    }

    // Record the attempt BEFORE calling store, so even failed lookups are counted.
    if (metrics_) metrics_->increment_gets();

    auto val = store_->get(request->key());

    if (val.has_value()) {
        // Cache HIT: found = true, value = the stored string.
        if (metrics_) metrics_->increment_hits();
        response->set_found(true);
        response->set_value(std::move(*val));   // std::move avoids a copy of the string.
    } else {
        // Cache MISS: Key not found or expired.
        // We return found=false. The caller is responsible for falling back to
        // the origin (e.g., querying the database directly — this is the "cache miss path").
        if (metrics_) metrics_->increment_misses();
        response->set_found(false);
    }

    return grpc::Status::OK;
}

// ============================================================================
// Delete Handler
// ============================================================================
// Used for cache invalidation: when a record in the database is updated,
// the application deletes the corresponding cache key to force a fresh lookup.
grpc::Status CacheServiceImpl::Delete(grpc::ServerContext* context,
                                      const DeleteRequest* request,
                                      DeleteResponse* response) {
    if (request->key().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
    }

    bool deleted = store_->del(request->key());
    response->set_deleted(deleted);

    // Even a miss-delete (key wasn't there) returns OK — it is idempotent.
    // The end result is the same: the key is not in the cache.
    return grpc::Status::OK;
}

// ============================================================================
// Exists Handler
// ============================================================================
// More efficient than Get when the caller only needs yes/no (e.g., checking
// if a rate-limit token bucket exists before creating it).
grpc::Status CacheServiceImpl::Exists(grpc::ServerContext* context,
                                      const ExistsRequest* request,
                                      ExistsResponse* response) {
    if (request->key().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
    }

    response->set_exists(store_->exists(request->key()));
    return grpc::Status::OK;
}

// ============================================================================
// Stats Handler
// ============================================================================
// Returns a snapshot of all cache metrics.
// This is useful for dashboards and for engineers investigating cache health
// without needing a full Prometheus setup (e.g., from a gRPC CLI tool).
grpc::Status CacheServiceImpl::Stats(grpc::ServerContext* context,
                                     const StatsRequest* request,
                                     StatsResponse* response) {
    const auto& m = store_->metrics();

    // Load all atomics. std::memory_order_seq_cst (default) ensures a consistent
    // snapshot — we don't want total_gets to reflect a different moment than cache_hits.
    response->set_total_gets(m.total_gets.load());
    response->set_total_sets(m.total_sets.load());
    response->set_cache_hits(m.cache_hits.load());
    response->set_cache_misses(m.cache_misses.load());
    response->set_evictions(m.evictions.load());
    response->set_total_keys(store_->size());
    response->set_hit_rate(m.hit_rate());

    return grpc::Status::OK;
}

}  // namespace distcache
