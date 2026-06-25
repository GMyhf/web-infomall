# Web InfoMall — Multi-stage Docker Build
#
# Stage 1: Builder — compile C++ Phase 2 binaries
# Stage 2: Runtime — minimal image with load + serve

# ── Stage 1: Builder ─────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    clang \
    libc++-dev \
    zlib1g-dev \
    make \
    && rm -rf /var/lib/apt/lists/*

COPY src/ /build/src/
WORKDIR /build/src

# Build load + serve binaries
# On Linux, iconv is part of glibc, so -liconv is not needed
RUN make CXX=clang++ CXX_STDLIB=/usr/include/c++/v1 LDFLAGS="-lz -lpthread"

# ── Stage 2: Runtime ─────────────────────────────────────────────
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/src/load /usr/local/bin/load
COPY --from=builder /build/src/serve /usr/local/bin/serve

EXPOSE 8088

VOLUME ["/data"]

ENTRYPOINT ["serve"]
CMD ["/data", "/data/index", "8088"]
