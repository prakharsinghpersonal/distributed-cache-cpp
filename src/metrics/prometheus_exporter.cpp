// ============================================================================
// prometheus_exporter.cpp — Prometheus Metrics Server Implementation
// ============================================================================
//
// This file brings the Prometheus exporter to life.
// Key design: ALL initialization happens in the CONSTRUCTOR.
// After construction, the HTTP metrics server is running in a background thread
// managed automatically by the prometheus-cpp library.
//
// MEMBER INITIALIZER LIST ORDER:
//   C++ initializes members in the ORDER they are DECLARED in the .h file,
//   not the order in the initializer list. This matters for us because
//   counter_family_ depends on registry_, and the counters depend on counter_family_.
//   We declare them in dependency order in the .h file to be safe.
//
// ============================================================================

#include "prometheus_exporter.h"
#include <prometheus/counter.h>
#include <prometheus/gauge.h>

namespace distcache {

MetricsExporter::MetricsExporter(const std::string& address)
    // Step 1: Bind an HTTP server to the given address.
    //         The Exposer spawns a background thread that listens for HTTP GET /metrics.
    //         When Prometheus scrapes us, this thread serves the registry_ data.
    : exposer_(std::make_unique<prometheus::Exposer>(address)),

    // Step 2: Create the metric registry.
    //         All counters/gauges you want to expose MUST be registered here.
    //         The registry owns the memory for all metrics.
      registry_(std::make_shared<prometheus::Registry>()),

    // Step 3: Define the counter family.
    //         BuildCounter() is a fluent builder:
    //           .Name()  → The metric base name in /metrics output.
    //           .Help()  → A human-readable description. Shown in Prometheus UI.
    //           .Register(*registry_) → Attach to registry and return a reference.
    //
    //         This one "family" will produce multiple lines in /metrics, one per label
    //         combination: type="get", type="set", type="hit", type="miss".
      counter_family_(prometheus::BuildCounter()
                          .Name("distcache_requests_total")
                          .Help("Total number of cache requests, labeled by operation type")
                          .Register(*registry_)),

    // Step 4: Add individual counters with label values.
    //         Each Add() call creates a distinct counter within the family.
    //         Labels are key-value pairs that Prometheus uses for filtering:
    //           distcache_requests_total{type="get"} 12345
    //         prometheus::Counter& is a reference — the counter lives in registry_.
      gets_counter_(counter_family_.Add({{"type", "get"}})),
      sets_counter_(counter_family_.Add({{"type", "set"}})),
      hits_counter_(counter_family_.Add({{"type", "hit"}})),
      misses_counter_(counter_family_.Add({{"type", "miss"}})),
      evictions_counter_(counter_family_.Add({{"type", "eviction"}})),

    // Step 5: Define a Gauge for the current number of cached keys.
    //         Unlike counters (monotonically increasing), gauges can decrease.
    //         The cache size goes up when we insert, down when we evict.
      gauge_family_(prometheus::BuildGauge()
                        .Name("distcache_keys_total")
                        .Help("Current number of live keys in the cache")
                        .Register(*registry_)),
      cache_size_gauge_(gauge_family_.Add({}))   // No labels needed for a single gauge.
{
    // Step 6: Register the registry with the exposer.
    //         Now when Prometheus scrapes http://host:9090/metrics, the exposer
    //         calls Collect() on the registry, gathers all counter/gauge values,
    //         and formats them into the Prometheus text exposition format.
    //
    //         We pass a weak_ptr to avoid a circular reference:
    //         exposer_ → registry_ (strong via RegisterCollectable)
    //         registry_ → (nothing back to exposer_)
    //         Using shared_ptr here would create a cycle → memory leak.
    exposer_->RegisterCollectable(registry_);
}

// ============================================================================
// Public increment methods
// ============================================================================
// prometheus::Counter::Increment() internally calls fetch_add on a std::atomic<double>.
// It is fully thread-safe and lock-free — multiple gRPC handler threads can call
// this simultaneously without any data races.

void MetricsExporter::increment_gets()      { gets_counter_.Increment(); }
void MetricsExporter::increment_sets()      { sets_counter_.Increment(); }
void MetricsExporter::increment_hits()      { hits_counter_.Increment(); }
void MetricsExporter::increment_misses()    { misses_counter_.Increment(); }
void MetricsExporter::increment_evictions() { evictions_counter_.Increment(); }

// set_cache_size(): Gauge::Set() replaces the current value atomically.
// This is called periodically from main.cpp's reporting loop, NOT from handlers.
// We deliberately avoid calling store_->size() from inside hot handler paths
// because size() acquires a lock on every shard — too expensive per-request.
void MetricsExporter::set_cache_size(double size) {
    cache_size_gauge_.Set(size);
}

}  // namespace distcache
