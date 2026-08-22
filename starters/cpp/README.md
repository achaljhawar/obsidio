# Obsidio C++ implementation

This directory contains the only runnable submission in the repository: a
C++17 HTTP service designed for the Obsidio two-CPU resilience workload.

Read the [root README](../../README.md) for project status and the
[challenge specification](../../docs/challenge/spec.md) for the normative API
and scoring rules. This document describes the implementation as it exists now.

## Build and run

From this directory:

```bash
docker build -t obsidio-cpp .
docker run --rm --cpus=2 --memory=2g -p 8080:8080 obsidio-cpp
```

The image is a pinned, multi-stage Debian build. Building it runs the hash
correctness suite four ways:

1. automatic backend selection;
2. forced reference fallback;
3. forced ARM backend, when available;
4. forced x86 SHA-NI backend, when available.

A forced backend is reported as skipped when the build CPU does not expose that
instruction set. On a CPU that does expose it, verification failure breaks the
image build.

### Linux host build

The server uses `epoll` and `eventfd`, so the host build is Linux-only. On
macOS and Windows, use Docker.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/obsidio-server
```

## Source layout

```text
src/main.cpp             Endpoint routing, configuration, startup, shutdown
src/http_server.*        Dependency-free HTTP/1.1 server using epoll/eventfd
src/data.*               Symbols, prices, immutable series, JSON rendering
src/risk_pool.*          Bounded queue and low-priority risk workers
src/risk.*               Backend selection, verification, chain entry points
src/chain_backend.hpp    One-to-four-lane backend interface
src/chain_arm.cpp        Runtime-gated ARMv8 SHA2 implementation
src/chain_x86.cpp        Runtime-gated x86 SHA-NI implementation
src/sha256.*             Generic SHA-256 and specialized 64-byte fallback
src/persist.*            Append-only price log and startup replay
tests/selftest.cpp       Build-gated hash and cross-lane correctness suite
tests/persist_test.sh    Destructive temporary-volume restart test
bench/                   Experimental benchmark sources, not default targets
```

## Request architecture

The workload mixes cheap lookups with an intentionally expensive hash chain.
Putting both on one execution path creates head-of-line blocking, so the server
separates them.

### IO path

`IO_THREADS` epoll loops own their connections. They execute `/health`,
`/price`, and `/stats` inline because those operations are small. `TCP_NODELAY`
is enabled for short JSON responses.

When an IO loop receives `/risk`, it transfers a job to the bounded risk queue,
removes the connection from epoll, and continues serving other sockets. The
connection returns to its owning loop through an `eventfd` completion queue.
Socket state is therefore not concurrently mutated by an IO thread and a risk
worker.

### Risk path

`RISK_WORKERS` threads execute at `SCHED_IDLE`, with `nice(19)` as a fallback.
Both mechanisms lower their priority; they do not require permission to raise
priority. When an IO thread becomes runnable, Linux can schedule it ahead of
the CPU-bound hash workers.

The pool consumes up to four queued jobs at a time and dispatches the widest
verified chain function available. Near queue drain it steps down through
three, two, and one lane rather than waiting for a full batch.

### Why interleaving matters

Each `/risk` request performs 50,000 dependent SHA-256 operations. One chain
cannot hide the latency of the CPU's SHA instructions. Advancing independent
chains in lockstep lets one lane issue useful work while another waits on its
dependency.

- ARMv8 has enough vector registers for a true four-lane interleave.
- x86-64 has 16 XMM registers and an implicit `XMM0` operand in
  `SHA256RNDS2`; the current kernel keeps two lanes resident, implements three
  lanes as pair-plus-single, and four lanes as two pairs.
- CPUs without a supported extension use the verified reference fallback.

The first hash accepts an arbitrary seed length. Every later hash receives the
previous 64-character hexadecimal digest, allowing specialized two-block
transforms and SIMD hexadecimal conversion on the hot path.

## Backend selection and correctness

At startup, `init_risk_backend()` selects a candidate for the current
architecture and verifies its lane functions against a deliberately plain
reference implementation. A failed core backend falls back; a failed wider
lane is disabled independently.

`RISK_BACKEND` can force a path for testing:

| Value | Behavior |
| --- | --- |
| unset | Select the best verified backend for the CPU |
| `reference` | Use the specialized serving fallback |
| `arm` | Request the ARMv8 SHA2 backend |
| `x86-sha-ni` | Request the x86 SHA-NI backend |

Examples:

```bash
RISK_BACKEND=reference ./build/obsidio-selftest
RISK_BACKEND=arm ./build/obsidio-selftest
RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest
```

Do not replace the reference oracle with the same primitive being tested. The
separation is intentional: sharing a buggy implementation could let the test
and candidate agree on the same wrong digest.

## Configuration

| Environment variable | Default | Meaning |
| --- | ---: | --- |
| `PORT` | `8080` | HTTP listen port |
| `IO_THREADS` | `2` | Independent epoll loops |
| `RISK_WORKERS` | `2` | CPU-bound risk worker threads |
| `RISK_QUEUE` | `512` | Maximum queued risk jobs |
| `RISK_DEADLINE_MS` | `0` | Queue-age rejection threshold; `0` disables it |
| `RISK_BACKEND` | automatic | Backend override used for verification and experiments |
| `PRICE_LOG` | `/data/prices.log` | Append-only persistence log path |

Thread counts are explicit because a container limited to two CPUs can still
see every host CPU. Sizing from `std::thread::hardware_concurrency()` would
oversubscribe the quota.

The default risk queue is deliberately deep. Every rejection consumes part of
the challenge's one-percent error budget; queue limits and deadlines should be
tuned from measured overload behavior, not guessed.

## API examples

```bash
curl http://127.0.0.1:8080/health
curl 'http://127.0.0.1:8080/price?symbol=AAPL'
curl 'http://127.0.0.1:8080/stats?symbol=AAPL'
curl 'http://127.0.0.1:8080/risk?seed=0.5'
curl -X POST http://127.0.0.1:8080/price \
  -H 'Content-Type: application/json' \
  -d '{"symbol":"AAPL","price":190.0}'
```

Valid symbols are `AAPL`, `GOOG`, `MSFT`, `AMZN`, `NVDA`, `META`, `TSLA`, and
`JPM`.

## Persistence behavior

`persist_init()` replays the log in file order, then opens it with `O_APPEND`.
An accepted update is appended as a small text record and followed by
`fdatasync`. A torn final record is ignored during replay.

Persistence is enabled only when `PRICE_LOG` is writable. The Docker image does
not create a volume, so the default run normally reports:

```text
persistence: DISABLED (/data/prices.log not writable)
```

Mount storage explicitly:

```bash
docker volume create obsidio-prices
docker run --rm --cpus=2 --memory=2g \
  -v obsidio-prices:/data -p 8080:8080 obsidio-cpp
```

The restart-survival test creates and removes a container and named volume with
fixed test names:

```bash
./tests/persist_test.sh
```

Passing that script proves the basic sequential restart path. It does not prove
correct behavior for concurrent updates or storage failures; those remain open
issues.

## Testing

### Correctness suite

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The suite covers FIPS SHA-256 vectors, the specialized 64-byte path, short and
full risk chains, rotated lane inputs, identical seeds, and agreement between
one-to-four-lane entry points. The test count is intentionally not documented;
the coverage should evolve without making the guide stale.

Regenerate a full-chain golden independently with Python:

```bash
python3 -c "import hashlib; h='0.5'; [h := hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)]; print(h)"
```

### Sanitizers

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug -DOBSIDIO_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Do not use the sanitizer build for performance measurements.

### Load test

With the service running:

```bash
cd ../../k6
k6 run -e TARGET=http://127.0.0.1:8080 grading.js
```

The checked-in script currently awards `work_score` for HTTP 200 responses; it
does not independently validate JSON bodies or require each scored request to
meet its tier latency. Treat the challenge prose as the intended contract and
add stricter checks before relying on the script as a correctness oracle.

## Measurement record

Results are kept as dated evidence because absolute scores depend heavily on
hardware and thermal state:

- [ARM strategy notes](../../docs/history/arm64-strategy-notes.md) — ARMv8
  optimization ladder and four-lane measurements.
- [x86 coarse audit](../../docs/history/x86-coarse-audit.md) — first native x86
  execution and controlled comparison with the reference fallback.
- [x86 session findings](../../docs/history/x86-session-findings.md) — fused
  two-lane work, head-to-head comparison, rejected AVX2 experiment, and thermal
  findings.

The current x86 kernel and the independently developed fused kernel were within
roughly one to two percent in alternated measurements, which was inside observed
run-to-run noise. Keep the implementation on `main`; keep the reports as the
evidence trail.

## Known gaps

Before treating the service as production-ready:

1. Make the memory update and durable append one ordered operation, and return
   errors when persistence fails.
2. Add overload, shutdown, persistence-failure, and concurrency tests.
3. Extend CI to native architecture coverage where accelerated instructions are
   expected to execute (the current runners are x86-64 without SHA-NI exposed,
   so the accelerated back ends are compiled but not executed there).

Closed by `fix/http-input-hardening`:

- `Content-Length` is parsed strictly and bodies are capped; see
  `tests/http_test.cpp` for the socket-level cases, which run as a Docker build
  gate and under CTest.
- Non-finite prices are rejected at the HTTP edge and again in `update_price`,
  which is the choke point the persistence replay also goes through.
- HTTP parsing and backpressure now have regression coverage; CI runs the image
  build, native CTest, and an ASan+UBSan pass.

These gaps do not invalidate the measured hash-kernel work, but they do define
the next engineering phase.
