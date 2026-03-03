// ============================================================================
// prometheus_exporter.h — Prometheus Observability Layer
// ============================================================================
//
// WHAT IS PROMETHEUS?
//   Prometheus is an open-source monitoring system. It works by "scraping"
//   (polling via HTTP GET) a /metrics endpoint on your service every N seconds.
//   The response is plain text with counters and gauges in a specific format.
//
//   Example /metrics output:
//     # HELP distcache_requests_total Total number of cache requests
//     # TYPE distcache_requests_total counter
//     distcache_requests_total{type="get"} 45312
//     distcache_requests_total{type="set"} 12004
//     distcache_requests_total{type="hit"} 41233
//     distcache_requests_total{type="miss"} 4079
//
//   Prometheus stores these time-series values and Grafana can visualize them
//   as real-time charts — showing hit rate, throughput, eviction rate, etc.
//
// WHY IS OBSERVABILITY CRITICAL? (RESUME STORY)
//   Before adding Prometheus, we had an "eviction race" bug:
//   Under load, the eviction counter was strangely low while the cache size
//   was also not growing. This seemed paradoxical until we added Prometheus:
//   We could see that `evictions` spiked ONLY when throughput spiked.
//   This revealed that evict_if_needed() was being called WITHOUT holding the
//   right lock, allowing two threads to simultaneously decide the shard was full,
//   both evict, and both insert → the total size never decreased as expected.
//   Prometheus made an invisible race condition VISIBLE.
//
// HOW prometheus-cpp WORKS:
//   1. We create a prometheus::Registry — the central collection of all metrics.
//   2. We define metric families using BuildCounter() / BuildGauge() etc.
//      A "family" is a group of metrics that share a name but differ by labels.
//   3. We add specific instances with label values:
//        counter_family_.Add({{"type", "hit"}}) → counter for hits
//        counter_family_.Add({{"type", "miss"}}) → counter for misses
//   4. We create a prometheus::Exposer that binds an HTTP server to a port.
//   5. We register the Registry with the Exposer.
//   6. The Exposer automatically serves /metrics on that port, pulling data
//      from the Registry whenever Prometheus scrapes it.
//
// WHY COUNTERS INSTEAD OF GAUGES?
//   - Counter: always increases. Perfect for tracking total requests, errors.
//     Prometheus can compute the rate of change (rate()) from absolute counters.
//   - Gauge: goes up AND down. Used for things like "current cache size".
//   We use counters for gets/sets/hits/misses because they never decrease.
//   We also expose a gauge for the current number of cached keys.
//
// ============================================================================

#pragma once

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <memory>
#include <string>

namespace distcache {

class MetricsExporter {
public:
    // address: The host:port to bind the Prometheus HTTP server.
    //          Default: "0.0.0.0:9090" — available on all network interfaces.
    //          "0.0.0.0" means "listen on every network interface," which is
    //          necessary for Docker/Kubernetes where the container IP is dynamic.
    explicit MetricsExporter(const std::string& address = "0.0.0.0:9090");

    // These methods are called from gRPC handlers to record activity.
    // Internally, they call counter.Increment() which is backed by a std::atomic.
    // Safe to call from any thread without external synchronization.
    void increment_gets();
    void increment_sets();
    void increment_hits();
    void increment_misses();
    void increment_evictions();

    // set_cache_size(): Updates the gauge reflecting the live number of keys.
    // Unlike counters, gauges can go up or down. This is called periodically
    // from a background thread in main.cpp.
    void set_cache_size(double size);

private:
    // exposer_: The HTTP server. It binds to the port in the constructor
    // and runs in a background thread managed by the prometheus-cpp library.
    // No manual thread management needed.
    std::unique_ptr<prometheus::Exposer> exposer_;

    // registry_: All metrics we define are registered here.
    // shared_ptr because Exposer needs a shared reference to pull data on demand.
    std::shared_ptr<prometheus::Registry> registry_;

    // counter_family_: A "family" groups multiple counters that share a metric name
    // but have different label values. This allows Prometheus to query:
    //   sum(distcache_requests_total)                    → total requests
    //   rate(distcache_requests_total{type="hit"}[5m])  → hits per second over 5 min
    prometheus::Family<prometheus::Counter>& counter_family_;

    // Individual counters — one per operation type.
    // prometheus::Counter& is a reference to a counter managed by the registry_.
    prometheus::Counter& gets_counter_;
    prometheus::Counter& sets_counter_;
    prometheus::Counter& hits_counter_;
    prometheus::Counter& misses_counter_;
    prometheus::Counter& evictions_counter_;

    // Gauge for current cache size.
    prometheus::Family<prometheus::Gauge>& gauge_family_;
    prometheus::Gauge& cache_size_gauge_;
};

}  // namespace distcache
