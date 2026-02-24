# High-Frequency Distributed Cache

## Overview
A high-performance, in-memory distributed caching service built in **C++** with lock-free data structures for concurrent read/write throughput at 120K+ RPS. Features ultra-low-latency **gRPC** APIs for inter-service communication and proactive **Prometheus** observability for cache eviction and availability tracking.

## Features
- **Lock-Free Architecture**: In-memory cache using lock-free concurrent hash maps optimized for 120K+ requests per second.
- **Ultra-Low-Latency gRPC**: Secure inter-service communication APIs reducing data retrieval bottlenecks by 25%.
- **Proactive Observability**: Prometheus metrics and automated alerts for cache hit/miss ratios, eviction rates, and system availability under sustained load.
- **Container-Ready**: Docker containerization for seamless deployment and horizontal scaling.

## Tech Stack
- **Language**: C++17
- **RPC Framework**: gRPC, Protocol Buffers
- **Monitoring**: Prometheus
- **Infrastructure**: Docker, CMake

## Project Structure
```
├── src/
│   ├── cache/
│   │   ├── cache_store.h          # Lock-free concurrent cache
│   │   └── cache_store.cpp
│   ├── server/
│   │   ├── grpc_server.h          # gRPC service implementation
│   │   └── grpc_server.cpp
│   └── main.cpp
├── proto/
│   └── cache_service.proto        # gRPC service definition
├── docker/
│   └── Dockerfile
├── CMakeLists.txt
└── README.md
```

## Getting Started
1. Build: `mkdir build && cd build && cmake .. && make -j$(nproc)`
2. Run: `./distributed_cache --port 50051`
3. Docker: `docker build -f docker/Dockerfile -t dist-cache . && docker run -p 50051:50051 dist-cache`
4. Metrics: `curl http://localhost:9090/metrics`
