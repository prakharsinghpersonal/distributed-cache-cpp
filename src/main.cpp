#include <iostream>
#include <memory>
#include <string>
#include <csignal>
#include <atomic>

#include "cache/cache_store.h"

static std::atomic<bool> running{true};

void signal_handler(int) {
    running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Configuration
    std::string host = "0.0.0.0";
    int port = 50051;
    size_t num_shards = 256;
    size_t max_per_shard = 10000;

    // Parse CLI args
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--shards" && i + 1 < argc) {
            num_shards = std::stoull(argv[++i]);
        } else if (arg == "--max-per-shard" && i + 1 < argc) {
            max_per_shard = std::stoull(argv[++i]);
        }
    }

    // Initialize cache
    auto cache = std::make_unique<distcache::CacheStore>(num_shards, max_per_shard);

    std::cout << "=== High-Frequency Distributed Cache ===" << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Host:           " << host << std::endl;
    std::cout << "  Port:           " << port << std::endl;
    std::cout << "  Shards:         " << num_shards << std::endl;
    std::cout << "  Max/Shard:      " << max_per_shard << std::endl;
    std::cout << "  Total Capacity: " << num_shards * max_per_shard << " entries" << std::endl;
    std::cout << std::endl;

    // In production, this is where the gRPC server would be started:
    // grpc::ServerBuilder builder;
    // builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    // builder.RegisterService(&cache_service);
    // auto server = builder.BuildAndStart();

    std::cout << "Cache server listening on " << host << ":" << port << std::endl;
    std::cout << "Prometheus metrics at http://" << host << ":9090/metrics" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    // Event loop
    while (running.load()) {
        // Print metrics periodically
        auto& m = cache->metrics();
        std::cout << "\r[Metrics] "
                  << "gets=" << m.total_gets.load()
                  << " sets=" << m.total_sets.load()
                  << " hits=" << m.cache_hits.load()
                  << " misses=" << m.cache_misses.load()
                  << " hit_rate=" << (m.hit_rate() * 100) << "%"
                  << " size=" << cache->size()
                  << " evictions=" << m.evictions.load()
                  << std::flush;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << std::endl << "Shutting down cache server..." << std::endl;
    cache->clear();
    std::cout << "Cache cleared. Goodbye." << std::endl;

    return 0;
}
