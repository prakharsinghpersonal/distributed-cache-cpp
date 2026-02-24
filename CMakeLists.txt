cmake_minimum_required(VERSION 3.16)
project(distributed_cache CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -pthread")

# Source files
add_executable(distributed_cache
    src/main.cpp
    src/cache/cache_store.cpp
)

target_include_directories(distributed_cache PRIVATE src)

# Optional: find and link gRPC + Protobuf
# find_package(Protobuf REQUIRED)
# find_package(gRPC REQUIRED)
# target_link_libraries(distributed_cache gRPC::grpc++ protobuf::libprotobuf)
