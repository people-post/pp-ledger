# Match GitHub Actions CI (ubuntu-latest → Ubuntu 24.04 / noble).
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libsodium-dev \
        nlohmann-json3-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_HTTP=ON \
    && cmake --build build -j"$(nproc)" \
    && strip \
        build/app/pp-beacon \
        build/app/pp-relay \
        build/app/pp-miner \
        build/app/pp-client \
        build/app/pp-http

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libsodium23 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder \
    /src/build/app/pp-beacon \
    /src/build/app/pp-relay \
    /src/build/app/pp-miner \
    /src/build/app/pp-client \
    /src/build/app/pp-http \
    /usr/local/bin/

# Run as root by default so bind-mounted data dirs are writable without a
# host-side chown. Tighten with --user / securityContext in production if needed.
WORKDIR /data

# Select a role at runtime, e.g.:
#   docker run ... pp-beacon -d /data
#   docker run ... pp-http --bind 0.0.0.0 --beacon beacon:8517 --miner miner:8518
CMD ["pp-client", "--help"]
