# Match GitHub Actions CI (ubuntu-latest → Ubuntu 24.04 / noble).
# Dependencies (pp-cpp-common / -crypto / -amp) come via FetchContent pins in cmake/.
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        git \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DPP_LEDGER_BUILD_HTTP=ON \
    && cmake --build build -j"$(nproc)" \
    && strip \
        build/app/pp-beacon \
        build/app/pp-relay \
        build/app/pp-miner \
        build/app/pp-client \
        build/app/pp-http

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# curl: the HEALTHCHECK (and probes from `docker compose exec`).
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder \
    /src/build/app/pp-beacon \
    /src/build/app/pp-relay \
    /src/build/app/pp-miner \
    /src/build/app/pp-client \
    /src/build/app/pp-http \
    /usr/local/bin/

# A non-root user owns the data directory; a named volume mounted there takes its ownership.
RUN useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin pp \
    && mkdir -p /data && chown pp:pp /data
USER pp
WORKDIR /data
VOLUME /data

# Container defaults (docs/ops/CONFIGURATION.md § Environment): the work directory is the volume,
# logs go to stderr only (the log driver collects them), and the probe endpoint listens inside the
# container (not published unless mapped).
ENV PP_LEDGER_WORK_DIR=/data \
    PP_LEDGER_LOG_FILE=none \
    PP_LEDGER_HEALTH_PORT=9090 \
    PP_LEDGER_HEALTH_BIND=0.0.0.0

# The nodes stop cleanly on SIGTERM (what `docker stop` sends).
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=15s --timeout=3s --start-period=30s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${PP_LEDGER_HEALTH_PORT}/healthz" >/dev/null || exit 1

# Select a role at runtime: pp-beacon, pp-relay, pp-miner, pp-http (see deploy/README.md).
CMD ["pp-client", "--help"]
