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

### Running the graded workload

With k6 installed on the host:

```bash
cd k6
k6 run -e TARGET=http://127.0.0.1:8080 grading.js
```

Nothing needs to be installed for the all-Docker form, which is how every
number in this repository was produced. The load generator is pinned to cores
the service does not use, and both sides talk over a user-defined bridge so no
host port proxy sits in the path:

```bash
docker network create obsidio-grade
docker run -d --name sut --network obsidio-grade --cpus=2 --memory=2g obsidio-cpp
docker run --rm --network obsidio-grade --cpuset-cpus=8-15 \
  -v "$(pwd)/k6:/scripts:ro" -e TARGET=http://sut:8080 \
  grafana/k6:latest run /scripts/grading.js
docker rm -f sut && docker network rm obsidio-grade
```

From Git Bash on Windows, prefix the `-v` command with `MSYS_NO_PATHCONV=1` and
use `$(pwd -W)`, or the mount path is rewritten and k6 runs an empty directory.

Adjust `--cpuset-cpus` to cores this machine actually has. The container and k6
compete for CPU when they share cores, which is fine for a smoke test and not
fine for a measurement: quote scores only from runs where they are separated,
on a cool machine, on mains power.

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
- Risk jobs are batched into independent chains advanced in lockstep, which
  exposes instruction-level parallelism without adding threads. The batch width
  follows the back end's real lane width — eight on x86 through the pipelined
  phase-split kernel, four on ARM — never the widest entry point that happens
  to exist.
- Runtime dispatch selects ARMv8 SHA2, x86 SHA-NI, or the specialized reference
  fallback. Accelerated paths verify themselves against a deliberately plain
  oracle before serving traffic, and a lane that fails is disabled on its own
  rather than taking the back end down with it.
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

- [Phase-split kernel](docs/phase-split-kernel.md) — the design brief for the
  eight-lane x86 kernel, with both retracted stages left in place.
- [Phase-split negative result](docs/phase-split-negative-result.md) — the
  sequential version measured at +6.2% against a +15% floor and rejected, then
  §6a on the pipelined variant that passed and shipped.
- [Wide block 2](docs/wide-block2-negative-result.md) — a projection that was
  built and measured at −23%, with the diagnostic that explains it.
- [Ryzen ceiling findings](docs/history/ryzen-ceiling-findings.md) — the
  session that established `SHA256RNDS2` as latency bound and closed the
  scheduler and `IO_THREADS` levers.
- [ARM strategy notes](docs/history/arm64-strategy-notes.md),
  [x86 coarse audit](docs/history/x86-coarse-audit.md), and
  [x86 session findings](docs/history/x86-session-findings.md) record the
  earlier ARM and x86 ladders.

On a Ryzen 7 170 under Docker Desktop the current tree grades at `work_score`
7,365,605 with all four latency thresholds green and no failed requests,
against 5,818,877 for the same tree before the eight-lane kernel. Both figures
are one laptop: absolute k6 scores move substantially with CPU model,
virtualization, power policy, and thermal state, so the portable claims are the
ratios and the reasoning, not the numbers.

## Known limitations

These are current engineering issues, not historical findings:

- The in-memory price is updated before the persistence append, append/sync
  failures are not surfaced, and concurrent updates are not ordered as one
  transaction. The current log is useful groundwork, not a completed durability
  guarantee.

Resolved, with regression coverage in `starters/cpp/tests/http_test.cpp`:

- `Content-Length` is parsed strictly with a saturating accumulator that cannot
  overflow; header and body sizes are capped (16 KiB / 64 KiB), conflicting
  duplicates are rejected, and malformed requests get 400/413/431 rather than a
  silent disconnect.
- `POST /price` rejects NaN, Infinity, and malformed numbers, so the cached
  price JSON can no longer contain `nan`. `update_price` enforces the same rule,
  which also covers the persistence replay path.
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
