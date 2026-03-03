// ============================================================================
// main.cpp — Entry Point: Wires Everything Together
// ============================================================================
//
// This file is the "orchestrator." It:
//   1. Parses command-line arguments (configuration).
//   2. Creates the CacheStore (sharded in-memory data store).
//   3. Creates the MetricsExporter (Prometheus HTTP server on :9090).
//   4. Creates the CacheServiceImpl (gRPC handler that uses 1 and 2).
//   5. Starts the gRPC server (listening on :50051 for client requests).
//   6. Runs a periodic reporting loop (prints metrics to stdout, updates Prometheus gauge).
//   7. Handles SIGINT/SIGTERM gracefully (Ctrl+C cleanly shuts down servers).
//
// SYSTEM DESIGN PERSPECTIVE:
//   This process runs as a single long-lived daemon. Multiple threads operate inside it:
//     - gRPC thread pool: N threads (auto-managed by gRPC) handle concurrent RPCs.
//     - Prometheus thread: 1 thread (managed by prometheus-cpp) serves /metrics HTTP.
//     - Main thread: runs the reporting loop and waits for shutdown signal.
//
//   All inter-thread communication happens through:
//     - CacheStore's sharded mutexes (for cache data).
//     - Atomic counters in CacheMetrics (for observability data).
//     - running atomic flag (for graceful shutdown signaling).
//
//   This is a textbook example of "shared nothing" design within a process:
//   threads don't share state EXCEPT through well-defined, thread-safe interfaces.
//
// GRACEFUL SHUTDOWN:
//   On SIGINT (Ctrl+C) or SIGTERM (kill command), we:
//     1. Set running = false — the main loop exits.
//     2. server->Shutdown() — gRPC stops accepting new requests, finishes in-flight ones.
//     3. Process exits — CacheStore and MetricsExporter destructors clean up memory.
//   This prevents data corruption from abrupt termination (e.g., half-written entries).
//
// ============================================================================

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#include <grpcpp/grpcpp.h>

#include "cache/cache_store.h"
#include "server/cache_service.h"
#include "metrics/prometheus_exporter.h"

// ============================================================================
// Graceful Shutdown via Signal Handling
// ============================================================================
// std::atomic<bool> running: The shutdown signal shared between the OS signal
// handler and the main thread's event loop.
//
// WHY std::atomic?
//   Signal handlers run on a separate execution context (the OS interrupts
//   the main thread). Writing to a non-atomic bool from a signal handler while
//   the main thread reads it is technically undefined behavior in C++.
//   std::atomic<bool> guarantees that the write is visible to the main thread
//   without data races — even across the signal/thread boundary.
static std::atomic<bool> running{true};

void signal_handler(int signum) {
    // SIGINT = 2 (Ctrl+C), SIGTERM = 15 (kill / Docker stop).
    // We simply flip the atomic flag. The main loop will notice and exit.
    // We avoid calling std::cout here — I/O functions are not async-signal-safe.
    (void)signum;  // Suppress unused parameter warning.
    running.store(false, std::memory_order_relaxed);
}

// ============================================================================
// main()
// ============================================================================
int main(int argc, char* argv[]) {
    // Register our signal handler for graceful shutdown.
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------
    // Defaults that make sense for a high-throughput cache server.
    std::string grpc_address  = "0.0.0.0:50051";  // gRPC service endpoint.
    std::string prom_address  = "0.0.0.0:9090";   // Prometheus /metrics endpoint.
    size_t      num_shards    = 256;               // 256 shards → minimal lock contention.
    size_t      max_per_shard = 10000;             // 256 * 10000 = 2.56M max entries.
    int         report_period = 5;                 // Print metrics every 5 seconds.

    // Simple CLI argument parsing.
    // In production, you'd use a proper flags library (e.g., gflags or CLI11).
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if      (arg == "--grpc-addr"   && i+1 < argc) grpc_address  = argv[++i];
        else if (arg == "--prom-addr"   && i+1 < argc) prom_address  = argv[++i];
        else if (arg == "--shards"      && i+1 < argc) num_shards    = std::stoull(argv[++i]);
        else if (arg == "--max-per-shard" && i+1 < argc) max_per_shard = std::stoull(argv[++i]);
    }

    // -------------------------------------------------------------------------
    // Startup Banner
    // -------------------------------------------------------------------------
    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║   High-Frequency Distributed Cache       ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";
    std::cout << "\nConfiguration:\n";
    std::cout << "  Shards:        " << num_shards << "\n";
    std::cout << "  Max/Shard:     " << max_per_shard << "\n";
    std::cout << "  Total Capacity:" << (num_shards * max_per_shard) << " entries\n";
    std::cout << "  gRPC Address:  " << grpc_address << "\n";
    std::cout << "  Prometheus:    http://" << prom_address << "/metrics\n\n";

    // -------------------------------------------------------------------------
    // Step 1: Create the CacheStore (the data layer)
    // -------------------------------------------------------------------------
    // We use shared_ptr so CacheStore can be jointly owned by:
    //   - CacheServiceImpl (to handle RPCs)
    //   - Main thread (to call size() for metrics reporting)
    // shared_ptr uses atomic reference counting — safe for multi-threaded ownership.
    auto cache = std::make_shared<distcache::CacheStore>(num_shards, max_per_shard);
    std::cout << "[1/4] Cache initialized with " << num_shards << " shards.\n";

    // -------------------------------------------------------------------------
    // Step 2: Create the Prometheus Metrics Exporter (the observability layer)
    // -------------------------------------------------------------------------
    // This constructor call:
    //   a) Creates the prometheus::Registry.
    //   b) Defines all counters and gauges.
    //   c) Starts an HTTP server background thread on prom_address.
    // After this line, http://prom_address/metrics is already serving traffic.
    auto metrics = std::make_shared<distcache::MetricsExporter>(prom_address);
    std::cout << "[2/4] Prometheus exporter serving on " << prom_address << "/metrics\n";

    // -------------------------------------------------------------------------
    // Step 3: Create the gRPC service implementation (the network layer)
    // -------------------------------------------------------------------------
    // CacheServiceImpl gets both shared_ptrs injected. It does NOT own them.
    // This is Dependency Injection — critical pattern for testability.
    distcache::CacheServiceImpl cache_service(cache, metrics);

    // -------------------------------------------------------------------------
    // Step 4: Build and Start the gRPC Server
    // -------------------------------------------------------------------------
    // ServerBuilder is a fluent builder for configuring the gRPC server.
    grpc::ServerBuilder builder;

    // AddListeningPort: binds a TCP socket on the given address.
    //   InsecureServerCredentials() → no TLS. In production, use SslServerCredentials().
    builder.AddListeningPort(grpc_address, grpc::InsecureServerCredentials());

    // RegisterService: tells gRPC to route incoming RPCs to cache_service.
    builder.RegisterService(&cache_service);

    // Tune the thread pool. SetSyncServerOption configures the thread pool size.
    // By default, gRPC creates one polling thread per CPU core.
    // For I/O bound workloads, more threads = more concurrency. For CPU-bound, keep at ncores.
    // builder.SetSyncServerOption(grpc::ServerBuilder::NUM_CQS, 4);

    // BuildAndStart() creates the server and starts listening in background threads.
    // unique_ptr means we own the server — it shuts down when server goes out of scope
    // or when we explicitly call Shutdown().
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "[3/4] gRPC server listening on " << grpc_address << "\n";

    std::cout << "[4/4] All systems operational. Ctrl+C to stop.\n\n";

    // -------------------------------------------------------------------------
    // Step 5: Main Event Loop — Periodic Metrics Reporting
    // -------------------------------------------------------------------------
    // This loop runs on the main thread. The gRPC server and Prometheus HTTP
    // server each run on their own background threads (managed by their libraries).
    // We just sit here, sleep, and occasionally print a metrics summary.
    int elapsed_seconds = 0;
    while (running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++elapsed_seconds;

        if (elapsed_seconds % report_period == 0) {
            const auto& m = cache->metrics();
            auto total_gets = m.total_gets.load(std::memory_order_relaxed);
            auto total_sets = m.total_sets.load(std::memory_order_relaxed);
            auto hits       = m.cache_hits.load(std::memory_order_relaxed);
            auto evictions  = m.evictions.load(std::memory_order_relaxed);
            auto hit_rate   = m.hit_rate() * 100.0;
            auto cache_size = cache->size();

            std::cout << "\r[t+" << elapsed_seconds << "s] "
                      << "gets="  << total_gets
                      << " sets=" << total_sets
                      << " hits=" << hits
                      << " hit_rate=" << static_cast<int>(hit_rate) << "%"
                      << " evictions=" << evictions
                      << " keys=" << cache_size
                      << "        " << std::flush;

            // Update the Prometheus cache size gauge with the current key count.
            // We do this here (periodically) rather than per-request to avoid
            // the O(N shards) cost of size() in the hot path.
            metrics->set_cache_size(static_cast<double>(cache_size));
        }
    }

    // -------------------------------------------------------------------------
    // Step 6: Graceful Shutdown
    // -------------------------------------------------------------------------
    // reached here only after SIGINT or SIGTERM.
    std::cout << "\n\nReceived shutdown signal. Draining in-flight requests...\n";

    // Shutdown with a deadline: wait up to 5 seconds for in-flight RPCs to finish.
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    server->Shutdown(deadline);

    // server->Wait() blocks until gRPC's internal thread pool has fully drained.
    // This ensures we don't exit while a handler is mid-execution.
    server->Wait();

    std::cout << "Cache cleared. Goodbye.\n";
    return 0;
}
