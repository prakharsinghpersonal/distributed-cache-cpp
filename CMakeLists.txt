cmake_minimum_required(VERSION 3.16)
project(distributed_cache CXX)

# ============================================================================
# Project Build Configuration
# ============================================================================
# C++17 is required for:
#   - std::optional<T>     → used in CacheStore::get() return type
#   - Structured bindings  → for (const auto& [key, value] : entries)
#   - std::shared_mutex    → readers-writer lock (C++17 in practice)
#   - std::invoke_result   → used internally by prometheus-cpp
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -O3: Maximum compiler optimization (vs -O0 debug, -O1 minimal, -O2 balanced).
#   Enables auto-vectorization, loop unrolling, inlining.
#   Critical for sustained 115K+ RPS — raw math on the hot path must be fast.
# -pthread: Link the POSIX multithreading library.
#   Required because std::thread, std::mutex, std::shared_mutex all rely on pthreads.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -pthread")

# Force use of protobuf@33 which is the version gRPC was compiled against.
# Using a mismatched version causes ABI (Application Binary Interface) breakage
# and segmentation faults at runtime due to incompatible struct layouts.
set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/protobuf@33" ${CMAKE_PREFIX_PATH})

# ============================================================================
# Proto Code Generation
# ============================================================================
# PROTO_SRC_DIR: Where our hand-written .proto files live.
# PROTO_BUILD_DIR: Where protoc will drop the auto-generated .pb.cc / .pb.h files.
set(PROTO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_BUILD_DIR})

# Specify the paths to generated files so CMake knows what targets to build.
set(PROTO_FILE ${PROTO_SRC_DIR}/cache.proto)
set(PROTO_SRCS ${PROTO_BUILD_DIR}/cache.pb.cc)
set(PROTO_HDRS ${PROTO_BUILD_DIR}/cache.pb.h)
set(GRPC_SRCS  ${PROTO_BUILD_DIR}/cache.grpc.pb.cc)
set(GRPC_HDRS  ${PROTO_BUILD_DIR}/cache.grpc.pb.h)

# Find gRPC. This automatically pulls in Protobuf, abseil, re2, and c-ares.
# CONFIG mode means: look for gRPCConfig.cmake (the modern CMake package file).
find_package(gRPC REQUIRED CONFIG)

# Locate the protoc binary from protobuf@33 (pinned to avoid ABI mismatch).
# NO_DEFAULT_PATH: Don't search /usr/bin or /opt/homebrew/bin — we want exactly v33.
find_program(Protobuf_PROTOC_EXECUTABLE protoc
    PATHS "/opt/homebrew/opt/protobuf@33/bin"
    NO_DEFAULT_PATH REQUIRED)

# grpc_cpp_plugin: A protoc "plugin" that, when invoked alongside protoc,
# generates the gRPC stub classes (CacheService::Service, CacheService::Stub).
# Without this plugin, protoc only generates data classes (SetRequest, etc.)
# but NOT the network transport layer.
find_program(gRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

# Find prometheus-cpp, installed via 'brew install prometheus-cpp'.
find_package(prometheus-cpp CONFIG REQUIRED)

# ============================================================================
# Proto Code Generation — Custom Build Command
# ============================================================================
# add_custom_command: tells CMake how to BUILD the proto output files.
# It runs protoc with two plugins:
#   --cpp_out:  generates cache.pb.cc / cache.pb.h (data classes)
#   --grpc_out: generates cache.grpc.pb.cc / cache.grpc.pb.h (service stubs)
# CMake re-runs this command whenever cache.proto changes.
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS
        --grpc_out=${PROTO_BUILD_DIR}
        --cpp_out=${PROTO_BUILD_DIR}
        -I ${PROTO_SRC_DIR}
        --plugin=protoc-gen-grpc=${gRPC_CPP_PLUGIN_EXECUTABLE}
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating C++ sources from cache.proto"
)

# ============================================================================
# Executable Target
# ============================================================================
add_executable(distributed_cache
    # Generated protobuf/gRPC code (auto-produced from cache.proto):
    ${PROTO_SRCS}
    ${GRPC_SRCS}

    # Hand-written application code:
    src/main.cpp                         # Entry point, wires everything together.
    src/cache/cache_store.cpp            # Sharded in-memory cache implementation.
    src/server/cache_service.cpp         # gRPC RPC handler implementations.
    src/metrics/prometheus_exporter.cpp  # Prometheus counter/gauge server.
)

# Include directories: tell the compiler where to find our .h files.
target_include_directories(distributed_cache PRIVATE
    src             # → finds "cache/cache_store.h", "server/cache_service.h", etc.
    ${PROTO_BUILD_DIR}  # → finds "cache.pb.h", "cache.grpc.pb.h" (generated files)
)

# Link libraries: tell the linker which .dylib/.a files to link against.
target_link_libraries(distributed_cache
    gRPC::grpc++            # Core gRPC runtime, channel, HTTP/2 transport.
    gRPC::grpc++_reflection # Enables grpcurl / Postman gRPC discovery of services.
    protobuf::libprotobuf   # Protobuf serialization runtime.
    prometheus-cpp::core    # Prometheus metric types (Counter, Gauge, etc.).
    prometheus-cpp::pull    # Prometheus HTTP "pull" exposure (the /metrics server).
)
