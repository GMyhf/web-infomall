# Web InfoMall — Multi-stage Docker Build
#
# Stage 1: Builder — compile C++ Phase 2 binaries
# Stage 2: Runtime — minimal image with load + serve

# ── Stage 1: Builder ─────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    g++ \
    zlib1g-dev \
    make \
    && rm -rf /var/lib/apt/lists/*

COPY src/ /build/src/
WORKDIR /build/src

# Build load + serve binaries
# On Linux, iconv is part of glibc, so -liconv is not needed.
RUN make clean && make -j"$(nproc)" load serve

# ── Stage 2: Runtime ─────────────────────────────────────────────
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/src/load /usr/local/bin/load
COPY --from=builder /build/src/serve /usr/local/bin/serve

EXPOSE 8088

VOLUME ["/archive"]

ENTRYPOINT ["serve"]
# --trusted-proxy-hops 0: this image publishes 8088 itself and docker-compose.yml
# maps it straight to the host, so nothing in front writes X-Forwarded-For. Leaving
# the binary's default of 1 here would only mean clients could forge the header and
# get a fresh rate-limit bucket per request. Raise it to match your topology if you
# put your own reverse proxy in front (see README, 速率限制).
CMD ["/archive/data", "/archive/index", "8088", "--trusted-proxy-hops", "0"]
