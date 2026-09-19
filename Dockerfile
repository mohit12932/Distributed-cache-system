# ── Stage 1: Builder ───────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /build_env

# Install build dependencies
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy source code and CMake configuration
COPY . .

# Build the project (Release mode)
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j $(nproc)

# ── Stage 2: Production Runtime ────────────────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /app

# Create a non-root user for security
RUN groupadd -r cachegroup && useradd -r -g cachegroup cacheuser \
    && mkdir -p /app/data \
    && chown -R cacheuser:cachegroup /app

# Switch to the non-root user
USER cacheuser

# Copy only the compiled binary from the builder stage
COPY --from=builder --chown=cacheuser:cachegroup /build_env/build/distributed_cache /app/distributed_cache

# Expose both the RESP cache port and the HTTP metrics port
EXPOSE 6379 8080

# Run the cache server
# Note: Provide the CACHE_AUTH_PASS environment variable at runtime
CMD ["/app/distributed_cache", "--port", "6379", "--http-port", "8080", "--mode", "write-back", "--data-dir", "/app/data"]