# Obsidio starter: C++

A C++17 port of the Obsidio endpoint contract. Unlike the four bundled
starters, this one is **not** deliberately naive. The architecture that keeps
the fast path fast was in place from the start, because that part is plumbing
rather than insight; the part that actually wins the track — making the hash
chain cheap — has since been built out in `src/chain_arm.cpp` and is worth
**2.96×** end to end against the OpenSSL baseline. See "Measured results".

## Layout

```
src/sha256.hpp/.cpp    SHA-256. Two backends: OpenSSL (default) or portable scalar.
src/chain_backend.hpp  Pluggable chain back end: chain1/chain2/chain3/chain4.
src/chain_arm.cpp      ARMv8 crypto-extension chain. Where the score comes from.
src/risk.cpp           Back-end selection + self-verification. The 50,000 rounds.
src/data.cpp           Symbols, 500-point series, JSON rendering.
src/http_server.cpp    Dependency-free epoll HTTP/1.1 server.
src/risk_pool.cpp      Bounded queue, SCHED_IDLE workers, x4..x1 lane batching.
src/main.cpp           Handlers and wiring.
tests/selftest.cpp     Correctness gate, 56 checks. Runs during docker build.
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

**Four chains run interleaved in one worker thread.** This is the piece that
produces the score, and it is the least obvious thing in the build. `sha256h`
has roughly 4-cycle latency but 1-cycle throughput, so a single serial chain
spends most of its time waiting on its own dependency and leaves the crypto
pipeline idle. Four *independent* chains advanced in lockstep ride along in
those gaps. Four chains take ~6.1 ms and return four answers, where one takes
3.4 ms and returns one.

This is instruction-level parallelism inside a single thread — **not** extra
threads and **not** extra cores. `RISK_WORKERS` is still 2, the container still
sits at exactly its 2-CPU quota, and peak memory under full load is 6.1 MiB of
the 2 GiB budget, because the lane state lives in NEON registers. The pool takes
up to four jobs off the queue and steps down through x3, x2, x1 as it drains.

**The back end verifies itself before it is selected.** `verify_backend()` in
`risk.cpp` reproduces reference digests for every lane count against five seeds
and six chain lengths, at startup. A back end that fails is rejected in favour
of the reference path, and the self-test turns rejection into a build failure. A
wrong digest scores zero and looks exactly like a right one.

**`-march=armv8-a+crypto` is scoped to `chain_arm.cpp` alone**, never the whole
target and never `-march=native`. Selection is gated on `HWCAP_SHA2` at runtime,
so the binary still starts on a CPU without the extension — it just serves from
the slower portable path.

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

## Measured results

Full k6 ramp under the real caps, `--cpus=2 --memory=2g`, container **unpinned**
exactly as the grader runs it, k6 on separate cores. aarch64 with the crypto
extensions. **Not** the grading hardware — reproduce this on the box you deploy
to, and read the ratios rather than the absolutes.

| Build | `work_score` | vs previous | `/risk` p95 | req/s |
|---|---|---|---|---|
| OpenSSL, two-block `sha256_64()` | 2,997,122 | — | 399 ms | 4,439 |
| ARMv8 crypto, x2 interleaved | 6,508,268 | **+117%** | 184 ms | 9,650 |
| ARMv8 crypto, x3 interleaved | 8,074,407 | **+24.1%** | 146 ms | 11,957 |
| **ARMv8 crypto, x4 interleaved** | **8,866,401** | **+9.8%** | **132 ms** | **13,140** |

**2.96× end to end**, every row clearing all four thresholds with 0.00% errors.
Score is proportional to requests served and requests served is bounded almost
entirely by `/risk` cost, so **halving the amortised hash cost roughly doubles
your score.**

Two results worth internalising before you write any code here:

**Hand-written intrinsics for one chain are worth about 3%.** Measured:
OpenSSL 3.48 ms/chain, hand-rolled ARMv8 crypto 3.38 ms/chain. OpenSSL's
assembly is already using the same instructions. If you write SHA intrinsics and
stop there, you have done the hard part and collected almost none of the prize.

**The whole win is the interleave.** x2 takes 1.77 ms/chain, x4 takes ~1.5 ms
amortised. The instruction was never the bottleneck — the dependency chain was.

## Where to optimise next

Most of the classic list is already done here: the specialised two-block
transform, ARMv8 crypto intrinsics with runtime dispatch, SIMD hex encoding via
`vqtbl1q_u8`, and multi-buffer interleaving. What is left, in order of payoff:

1. **The persistence bonus.** `POST /price` is not in the k6 mix, so appending
   each update to a file on a mounted volume and replaying at startup costs
   essentially nothing during the graded run. This is the largest uncollected
   item on the board — real points for an afternoon, against low single digits
   from further chain work.
2. **An x86 SHA-NI back end**, if you are deploying to x86. `chain_backend.hpp`
   is the seam: implement the four `chain*` functions and return them from a
   `sha_ni_backend()`. On an x86 grading box the ARM back end simply fails its
   runtime gate and the OpenSSL path serves — correct, but ~3× slower.
3. **More lanes: measured and rejected.** x5 scored +0.63% over x4, against a
   run-to-run variance of 0.075%, so the gain is real but tiny. A `Lane` is six
   128-bit vectors, so four lanes hold 24 of the 32 NEON registers and five want
   30 — spill starts cancelling the batching win exactly where the register file
   predicts. Each added lane also costs ~22 µs on `/price` p95, since a worker
   commits to more chains before it yields. Do not revisit without new evidence.
4. **The fast path.** `/price` and `/stats` sit at ~500 µs p95 against 200 ms and
   500 ms bars. There is nothing to win. Do not be tempted.

The loop is allocation-free and free of per-call API overhead, so there is no
garbage-collection-style win left to claim either — those are banked.

## Do not break the digest

`tests/selftest.cpp` runs 56 checks: FIPS 180-4 vectors, short chains, the full
50,000-round result for fixed seeds, and — for every lane count — per-lane
digests, rotated seed order, all-same-seed, and cross-checks that x4 agrees with
x3, x2 and x1 on the lanes they share. It runs as a **build step** in the
Dockerfile: if any digest is ever wrong, the image does not build.

That last category is the one people skip and should not. Without it the digest
you serve can depend on how deep the queue happened to be when your request
arrived — a bug that is invisible under light load and catastrophic under the
graded ramp.

This matters more than it looks. A wrong digest still returns a plausible
64-char hex string, the grader verifies it against the seed, and that request
scores **zero**. Every optimisation above is a chance to silently break the
chain. Keep the gate.

Two more guards worth keeping: `verify_backend()` refuses to select an
accelerated back end that cannot reproduce the reference digests, and the
self-test fails the build if an accelerated back end was available but rejected
— so you can never ship quietly slow *or* quietly wrong.

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
