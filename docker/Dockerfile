FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir build && cd build && cmake .. && make -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y libgrpc++1 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /app/build/distributed_cache .

EXPOSE 50051 9090
ENTRYPOINT ["./distributed_cache"]
