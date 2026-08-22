# Obsidio

Obsidio is a performance-engineered C++17 analytics API built for a resilience
challenge: keep inexpensive requests responsive while CPU-heavy SHA-256 work
saturates a container limited to two CPUs and 2 GiB of memory.

This repository now contains one implementation—the C++ submission—and the
published challenge material, measurement history, and grading script provided
to evaluate it.

> **Current status:** the accelerated ARM and x86 risk paths are implemented and
> build-gated for correctness. The repository is not yet production-hardened;
> the open correctness and durability issues are listed below.

## Start here

- [Challenge specification](docs/challenge/spec.md) — endpoint contract,
  resource limits, scoring, and deliverables.
- [C++ implementation guide](starters/cpp/README.md) — architecture, build,
  configuration, testing, and persistence details.
- [Resilience write-up](docs/resilience-writeup.md) — submission narrative,
  measured decisions, trade-offs, and remaining risks.
- [Documentation index](docs/README.md) — current documents and historical
  evidence, with their status explained.
- [Grading script](k6/grading.js) — the checked-in k6 workload.

## Repository layout

```text
.
├── README.md
├── docs/
│   ├── README.md
│   ├── challenge/              Original challenge material
│   └── history/                Dated experiments and superseded plans
├── k6/
│   └── grading.js              Published load generator
└── starters/cpp/
    ├── CMakeLists.txt
    ├── Dockerfile
    ├── README.md
    ├── bench/                  Experimental benchmark sources
    ├── src/                    Service implementation
    └── tests/                  Correctness and restart-survival tests
```

Generated build trees and benchmark binaries are intentionally not tracked.

## Quick start

From the repository root:

```bash
docker build -t obsidio-cpp starters/cpp
docker run --rm --cpus=2 --memory=2g -p 8080:8080 obsidio-cpp
```

In another terminal:

```bash
curl http://127.0.0.1:8080/health
curl 'http://127.0.0.1:8080/price?symbol=AAPL'
curl 'http://127.0.0.1:8080/stats?symbol=AAPL'
curl 'http://127.0.0.1:8080/risk?seed=0.5'
```

Run the published workload after the container becomes healthy:

```bash
cd k6
k6 run -e TARGET=http://127.0.0.1:8080 grading.js
```

The container and k6 compete for CPU if they run on the same machine. That is
fine for a smoke test, but performance results should be collected with the
load generator on separate cores or a separate host.

## Endpoint contract

| Endpoint | Purpose | Success response |
| --- | --- | --- |
| `GET /health` | Liveness | `{"status":"ok"}` |
| `GET /price?symbol=SYM` | Current in-memory price | `{"symbol":"AAPL","price":187.42}` |
| `GET /stats?symbol=SYM` | Mean, min, max, and population standard deviation over 500 points | JSON statistics object |
| `GET /risk?seed=VALUE` | 50,000-round SHA-256/hex feedback chain | JSON seed and final digest |
| `POST /price` | Optional price update and persistence path | Updated symbol and price |

The fixed symbols are `AAPL`, `GOOG`, `MSFT`, `AMZN`, `NVDA`, `META`, `TSLA`,
and `JPM`. Unknown symbols return `404` with `{"error":"unknown symbol"}`.

See the [challenge specification](docs/challenge/spec.md) for the normative
contract.

## Architecture at a glance

The service separates cheap request handling from expensive risk computation:

```text
connections
    │
    ▼
Linux epoll IO threads ────────► /health, /price, /stats
    │
    └── /risk ──► bounded queue ──► low-priority risk workers
                                      │
                                      └──► eventfd completion back to IO
```

- Two explicitly configured epoll threads own sockets and serve the fast paths.
- Two `SCHED_IDLE` risk workers consume a bounded queue, allowing runnable IO
  work to pre-empt hashing.
- Risk jobs are batched into one-to-four independent chains to expose
  instruction-level parallelism without adding threads.
- Runtime dispatch selects ARMv8 SHA2, x86 SHA-NI, or the specialized reference
  fallback. Accelerated paths verify themselves against a deliberately plain
  oracle before serving traffic.
- The Docker build runs the correctness suite under automatic selection, the
  reference fallback, and each named accelerated backend when available.

The implementation guide explains the design and its trade-offs in detail.
The [resilience write-up](docs/resilience-writeup.md) connects those decisions
to the measured challenge results.

## Persistence

The service contains an append-only price log. When persistence is enabled, it
replays the log at startup, then appends and calls `fdatasync` after each
accepted `POST /price` before responding. The default path is
`/data/prices.log` and can be changed with `PRICE_LOG`.

The image does not create or declare a persistent volume. Without a writable
`/data` mount, startup logs that persistence is disabled and updates remain
in memory:

```bash
docker volume create obsidio-prices
docker run --rm --cpus=2 --memory=2g \
  -v obsidio-prices:/data -p 8080:8080 obsidio-cpp
```

There is deliberately no Compose file in the current tree. The previous one
targeted a deleted Node/Postgres system and did not describe this submission.

## Performance evidence

The repository records results rather than presenting one hardware-dependent
number as universal:

- [ARM strategy notes](docs/history/arm64-strategy-notes.md) record the original
  ARMv8 four-lane optimization ladder.
- [x86 coarse audit](docs/history/x86-coarse-audit.md) records the first real
  x86 execution, correctness checks, and controlled backend comparison.
- [x86 session findings](docs/history/x86-session-findings.md) record the fused
  two-lane SHA-NI work, head-to-head comparison, and thermal-throttling lesson.

The strongest portable conclusion is architectural: interleaving independent
chains removes dependency stalls. Absolute k6 scores vary substantially with
CPU model, virtualization, power policy, and thermal state.

## Known limitations

These are current engineering issues, not historical findings:

- The HTTP parser trusts an unchecked `Content-Length`; an extreme value can
  overflow size arithmetic and terminate the process. A body-size limit and
  strict integer parsing are still required.
- `POST /price` accepts non-finite floating-point values, which can produce
  invalid JSON such as `inf`.
- The in-memory price is updated before the persistence append, append/sync
  failures are not surfaced, and concurrent updates are not ordered as one
  transaction. The current log is useful groundwork, not a completed durability
  guarantee.
- `k6/grading.js` increments `work_score` for HTTP 200 responses; it does not
  independently validate response bodies or enforce a per-request latency test
  before awarding work.
- Automated coverage is concentrated on SHA-256 and backend equivalence. HTTP
  parsing, overload behavior, shutdown, persistence failures, and concurrent
  updates need integration tests and CI.
- The server uses Linux `epoll` and `eventfd`. Build and run it in Docker on
  macOS or Windows; the host CMake build is currently Linux-only.

## Development checks

On Linux:

```bash
cmake -S starters/cpp -B starters/cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build starters/cpp/build -j
ctest --test-dir starters/cpp/build --output-on-failure
```

The Docker build is the portable project gate because it uses the same Linux
environment on every host:

```bash
docker build --no-cache -t obsidio-cpp starters/cpp
```

For deeper commands, including sanitizers and the restart-survival test, see
the [C++ implementation guide](starters/cpp/README.md).
