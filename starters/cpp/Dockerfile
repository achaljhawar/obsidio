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
# The x86 measurement binaries build alongside the server so they are always
# available from the build stage, and so they cannot silently rot: a benchmark
# that stops compiling is a benchmark nobody runs.
COPY bench ./bench

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# Correctness gate. FIPS vectors plus the full 50,000-round chain.
#
# FOUR passes. The first exercises whatever accelerated back end this build
# host offers; the second forces the serving fallback (chain_fallback ->
# sha256_64) so it cannot ship untested. The last two force each accelerated
# back end by name: on a host whose CPU lacks that back end the selftest
# SKIPs (exit 0), but on a host that advertises it -- the x86 grading box
# included, since the grader builds this image on its own machine -- a
# verification failure becomes a BUILD failure instead of a silent ~28%
# fallback at runtime.
RUN ./build/obsidio-selftest \
    && RISK_BACKEND=reference ./build/obsidio-selftest \
    && RISK_BACKEND=arm ./build/obsidio-selftest \
    && RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest

# Input-hardening gate: raw-socket tests for Content-Length overflow, header
# and body caps, conflicting/decoy headers, and non-finite prices. A parser
# regression here is a crash or a mis-framed request under a hostile client,
# neither of which the digest self-test above can see.
RUN ./build/obsidio-http-test ./build/obsidio-server

FROM debian:bookworm-20240926-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3=3.0.20-1~deb12u2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /app/build/obsidio-server /usr/local/bin/obsidio-server

# Sized for the 2-CPU cap, NOT for the host core count the container can see.
ENV IO_THREADS=2 \
    RISK_WORKERS=2 \
    RISK_QUEUE=512 \
    RISK_DEADLINE_MS=0

EXPOSE 8080
CMD ["/usr/local/bin/obsidio-server"]
