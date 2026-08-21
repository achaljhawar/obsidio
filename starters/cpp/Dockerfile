# Obsidio starter: C++. Multi-stage build, pinned images, small final image.
#
# The correctness self-test runs as part of the build: if the /risk digest is
# ever wrong, the image does not build. A wrong digest scores zero, and the
# failure is otherwise silent, so this gate is deliberate. Do not remove it.
FROM debian:bookworm-20240926-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      g++=4:12.2.0-3 \
      cmake=3.25.1-1 \
      make=4.3-4.1 \
      libssl-dev=3.0.20-1~deb12u2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# Correctness gate. FIPS vectors plus the full 50,000-round chain.
#
# Run TWICE. The first pass exercises whatever accelerated back end this build
# host offers; the second forces the serving fallback (chain_fallback ->
# sha256_64) against the same golden digests. Without the second pass the
# fallback -- the only path an x86 or crypto-less grading box would ever run --
# ships untested.
RUN ./build/obsidio-selftest \
    && RISK_BACKEND=reference ./build/obsidio-selftest

FROM debian:bookworm-20240926-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3=3.0.20-1~deb12u2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /app/build/obsidio-server /usr/local/bin/obsidio-server

# Persistence bonus. /data is where POST /price updates are logged and
# replayed from. Declared as a volume so even a plain `docker run` gets an
# anonymous volume that survives `docker restart`; the compose file mounts a
# named volume here to survive the stricter rm + re-run case.
RUN mkdir -p /data
VOLUME /data

# Sized for the 2-CPU cap, NOT for the host core count the container can see.
ENV IO_THREADS=2 \
    RISK_WORKERS=2 \
    RISK_QUEUE=512 \
    RISK_DEADLINE_MS=0 \
    PRICE_LOG=/data/prices.log

EXPOSE 8080
CMD ["/usr/local/bin/obsidio-server"]
