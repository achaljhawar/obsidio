# Obsidio starter: C++

A C++17 port of the Obsidio endpoint contract. Unlike the four bundled
starters, this one is **not** deliberately naive: the architecture that keeps
the fast path fast is already in place, because that part is plumbing rather
than insight. The part that actually wins the track — making the hash chain
cheap — is left as a clearly marked hook in `src/risk.cpp`.

## Layout

```
src/sha256.hpp/.cpp    SHA-256. Two backends: OpenSSL (default) or portable scalar.
src/risk.cpp           The 50,000-round chain. ~98% of all CPU. Optimise HERE.
src/data.cpp           Symbols, 500-point series, JSON rendering.
src/http_server.cpp    Dependency-free epoll HTTP/1.1 server.
src/risk_pool.cpp      Bounded queue + SCHED_IDLE worker threads.
src/main.cpp           Handlers and wiring.
tests/selftest.cpp     Correctness gate. Runs during docker build.
```

## Build and run under the real caps

```
docker build -t obsidio-cpp .
docker run --rm --cpus=2 --memory=2g -p 8080:8080 obsidio-cpp
curl http://127.0.0.1:8080/health
```

Locally, without Docker (Linux only — the server uses `epoll`):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/obsidio-selftest && ./build/obsidio-server
```

On macOS only the computation layer builds; the server needs Linux.

## The design, and why

**Two IO threads, two risk workers.** All sized from env vars, never from
`std::thread::hardware_concurrency()` — the container is capped at 2 CPUs but
can see every host core, and sizing from the visible count is the documented
trap in this track.

**`/price` and `/stats` run inline on the IO thread.** They are microseconds of
work; queueing them would cost more than doing them.

**`/risk` is never computed on an IO thread.** It goes to a bounded queue. The
connection is removed from the epoll set while the work is in flight and handed
back through an `eventfd` when the digest is ready, so connection lifetime stays
on one thread and there is no cross-thread socket race.

**Risk workers run at `SCHED_IDLE`.** This is the piece that solves head-of-line
blocking, and it is the main reason to be in C++ rather than Go. When an IO
thread becomes runnable the kernel preempts a hashing worker essentially
immediately, instead of letting it run out a full CFS timeslice. Both
`sched_setscheduler(SCHED_IDLE)` and the `nice(19)` fallback are permitted for
unprivileged processes in a container — lowering your own priority never needs
`CAP_SYS_NICE`.

**TCP_NODELAY is on.** Nagle would add tens of milliseconds to small responses.

## Tuning knobs

| Env var | Default | Notes |
|---|---|---|
| `PORT` | `8080` | Required by the grader |
| `IO_THREADS` | `2` | epoll loops |
| `RISK_WORKERS` | `2` | Hash workers, `SCHED_IDLE` |
| `RISK_QUEUE` | `512` | Queue full ⇒ 503. Deep on purpose (see below) |
| `RISK_DEADLINE_MS` | `0` | `0` disables. Sheds jobs that already blew their budget |

**On shedding.** k6 counts any status ≥ 400 against a **1% error ceiling**, so
you can shed roughly 1 request in 100 and no more. The default queue is deep
enough that it should never fill under the graded load; if it does, that is a
signal to investigate, not a knob to turn up. `RISK_DEADLINE_MS` is off by
default for the same reason — dropping a doomed request frees CPU for requests
that can still land in time, but every drop spends part of your error budget.

## Measured baseline

Single core, Apple M-series (ARM crypto extensions), `-O3`. **Not** the grading
hardware — reproduce this on the box you actually deploy to:

| SHA-256 backend | `/risk` per request | Implied 2-core mix ceiling |
|---|---|---|
| Portable scalar | 17.67 ms | ~1,130 req/s |
| OpenSSL (hardware) | **9.10 ms** | **~2,200 req/s** |

That 1.94× is the entire argument for this track in one line. Score is
proportional to requests served, and requests served is bounded almost entirely
by `/risk` cost, so **halving the hash cost roughly doubles your score.**
OpenSSL is the default backend for exactly this reason.

**x86-64** (AMD Ryzen 7 6800HS, WSL2 Ubuntu 24.04, `-O3`, raw process — not
yet under a `--cpus=2` container, see the caveat in `STRATEGY.md` section 12):

| SHA-256 backend | ms/chain | chains/s/core |
|---|---:|---:|
| OpenSSL (per-call) | 16.6-17.0 | ~60 |
| `chain_x86.cpp` (SHA-NI, two-block specialised) | **4.20-4.23** | **~238** |

**~4.0×** on top of the already-hardware-accelerated baseline, from skipping
the generic padding path and OpenSSL's per-call setup on every one of the
50,000 iterations. Measured with `bench/bench_time.cpp` (a standalone A/B
harness — compile with `g++ -std=c++17 -O3 -DOBSIDIO_USE_OPENSSL -msse4.1
-mssse3 -msha -I src src/{sha256,risk,chain_arm,chain_x86,data}.cpp
bench/bench_time.cpp -lssl -lcrypto -lpthread -o bench/bench_time`, then run
it once with `RISK_BACKEND=reference` and once with `RISK_BACKEND=x86-sha-ni`).

## Where to optimise next

In `src/risk.cpp`, in order of expected payoff:

1. **A specialised two-block transform.** Done, both back ends. After the
   first round the input is always exactly 64 bytes, so every hash is two
   SHA-256 blocks and the second block is a compile-time constant (`0x80`,
   zeros, length = 512 bits) — its message schedule is precomputed, so the
   generic padding path never runs in the steady state.
2. **SHA-NI / ARMv8 crypto intrinsics directly**, with runtime CPU dispatch and
   the portable path as fallback.
   - **ARM (`src/chain_arm.cpp`): done, measured, auto-selected.** See the
     baseline table below.
   - **x86 (`src/chain_x86.cpp`): done, measured, auto-selected.** It was
     written with no x86 machine available to compile or run it on, so it
     shipped initially gated behind `RISK_BACKEND=x86-sha-ni`. Verified
     2026-08-22 on an AMD Ryzen 7 6800HS under WSL2: all 21 selftest digest
     checks pass, ~4.0x the OpenSSL-per-call path (see the baseline table and
     `STRATEGY.md` section 12). It is now unconditional, same as the ARM
     back end. Re-verify after any future change to that file:
     ```
     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
     cmake --build build -j
     RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest
     ```
3. **SIMD hex encoding** — 32 bytes to 64 chars with `pshufb`, not a byte loop.
   Done on ARM (fused straight into the next iteration's message vectors, no
   store/reload). The x86 back end currently hex-encodes through the portable
   `hex_encode()` instead of a fused NEON-style table lookup — a smaller,
   safer piece to hand-verify once no compiler is available, at a small cost
   in throughput once it is confirmed correct.
4. **Multi-buffer SHA-256.** You cannot parallelise one chain, but you can
   advance 8 concurrent requests' chains in lockstep under AVX2. Biggest win
   available, biggest engineering cost, and x86-only. Not attempted.

The loop is already allocation-free, so there is no garbage-collection-style
win left to claim here — that one is already banked.

## Do not break the digest

`tests/selftest.cpp` checks FIPS 180-4 vectors, short chains, and the full
50,000-round result for fixed seeds. It runs as a **build step** in the
Dockerfile: if the digest is ever wrong, the image does not build.

This matters more than it looks. A wrong digest still returns a plausible
64-char hex string, the grader verifies it against the seed, and that request
scores **zero**. Every optimisation above is a chance to silently break the
chain. Keep the gate.

Regenerate any golden value with:

```
python3 -c "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
```

## Sanitised build

Run the k6 script against a sanitised build at least once before you trust the
release build:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOBSIDIO_SANITIZE=ON
cmake --build build-asan -j && ./build-asan/obsidio-server
```

ASan and UBSan turn silent memory corruption into a loud stack trace, which is
what makes C++ survivable on a deadline. Never ship this build — it is roughly
2× slower.

## Persistence bonus

`POST /price` is implemented but **in memory only**, so it earns no bonus as
written. To claim it, append each update to a file on a mounted volume and
replay it at startup. `POST /price` is not in the graded load mix at all, so the
write cost is essentially free during the run — you do not need Postgres, and
adding it would spend CPU from the shared 2-core budget for nothing.
