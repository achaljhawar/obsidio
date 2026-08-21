# Obsidio: strategy and working notes

Everything worked out so far: what the track actually rewards, where the
leverage is, what has been built, what is verified, and what to do next.

Track rules live in `OBSIDIO-DETAIL-PAGE.md`, `directions.md`, and `track.md`.
The grading script is `k6/grading.js`. This file is the plan, not the rules.

---

## 1. The one thing to understand

Score is `1×price + 3×stats + 10×risk`, but **the request mix is decided by the
client**, independently per iteration, 60/30/10. Nothing you build changes it.
So over a run you always earn ~250 points per 100 requests served:

```
score ≈ 2.5 × (total requests served)
```

Total requests served is bounded by CPU, and `/risk` eats essentially all of it.
Per 100 requests: price ≈ 0.6 ms, stats ≈ 0.6 ms, risk ≈ 10 × `risk_ms`. Risk is
~98% of compute. Therefore:

```
throughput ≈ 20,000 / risk_ms   requests/sec on 2 cores
```

**Halving the CPU cost of the hash chain roughly doubles the score.** Nothing
else on the board is close. Everything else is about clearing the gate.

### Two separate jobs

| Job | Problem type | What solves it |
|---|---|---|
| **Qualify** — `/price` p95 < 200 ms | Queueing | Admission control, priority, never block the IO thread |
| **Win** — maximise score | Throughput | Make one SHA-256 chain cheaper |

Both are mandatory. Qualifying gets you on the leaderboard; only then does the
score count. They are solved by completely different code.

---

## 2. Non-obvious things read out of the grading script

**The k6 iteration has no `sleep()`.** It is a *closed loop*: each VU fires the
next request the instant the previous returns. Offered load is therefore your
own throughput, and the box sits pinned at saturation for the whole peak. This
behaves very differently from "handle N req/s" — the run queue is never empty,
and the real question is who waits.

**Load shedding is a trap.** The track prose praises shedding overflow, but
k6's `http_req_failed` counts any status ≥ 400 against a **1% error ceiling**.
You can shed about 1 request in 100 and no more. The lever is admission control
and prioritisation, not rejection.

**Caching is useless here, by design.** `/risk` has a random seed per request.
`/stats` is explicitly forbidden from caching. `/price` is already an in-memory
map. The standard "make it fast" instinct has nothing to grab. Discard it.

**The persistence bonus is nearly free.** The docs frame it as a painful
tradeoff because they assume Postgres. But `POST /price` **is not in the k6 mix
at all** — the script only issues GETs. So persistence costs approximately zero
CPU during the graded run. Single container, a named volume, append each update
to a file with an fsync, replay at boot. No database, no shared-budget tax.

**`/price` p95 is the headline.** A price lookup does no work. If it is slow, it
is queued behind heavy requests. That number is a direct readout of your
architecture.

---

## 3. Language decision

Settled: **C++**. Reasoning kept here because it should inform the pitch.

The SHA-256 compression itself runs at identical speed in any language — it is a
hardware instruction (SHA-NI on x86, ARMv8 crypto extensions on ARM), and Go's
stdlib already dispatches to assembly that uses it. C++ wins on three things:

1. **A specialised two-block hash.** After round one the input is always exactly
   64 bytes, so every hash is two SHA-256 blocks and the second is a
   compile-time constant. Go can't express this — `crypto/sha256`'s `block()` is
   unexported, so you pay `Sum256`'s generic setup every call.
2. **Multi-buffer SHA-256** — 8 chains in lockstep under AVX2. x86 only (see the
   Spark section below).
3. **OS thread priority.** `sched_setscheduler(SCHED_IDLE)` on the hash workers
   means the kernel preempts them the instant an IO thread is runnable. This
   solves head-of-line blocking *at the scheduler* rather than by sprinkling
   yield calls. Go can only approximate it (`LockOSThread` + `setpriority`).

What C++ costs: development time, and a failure mode where mistakes are silent
memory corruption instead of a stack trace. Both are mitigated below.

---

## 4. What has been built

`starters/cpp/` — ~1,540 lines, C++17, no dependencies except OpenSSL.

```
src/sha256.hpp/.cpp    SHA-256. Two backends: OpenSSL (default) or portable scalar.
src/risk.cpp           The 50,000-round chain. ~98% of all CPU. Optimise HERE.
src/data.cpp           Symbols, 500-point series, JSON rendering.
src/http_server.cpp    Dependency-free epoll HTTP/1.1 server.
src/risk_pool.cpp      Bounded queue + SCHED_IDLE worker threads.
src/main.cpp           Handlers and wiring.
tests/selftest.cpp     Correctness gate. Runs during docker build.
```

### Design

- **Two IO threads, two risk workers**, sized from env vars, never from
  `std::thread::hardware_concurrency()` — the container is capped at 2 CPUs but
  sees every host core. That is the documented trap in this track.
- **`/price` and `/stats` run inline on the IO thread.** Microseconds of work;
  queueing them would cost more than doing them.
- **`/risk` never runs on an IO thread.** It goes to a bounded queue. The
  connection leaves the epoll set while hashing and is handed back through an
  `eventfd`, so connection lifetime stays on one thread with no cross-thread
  socket race.
- **Risk workers run at `SCHED_IDLE`**, with a `nice(19)` fallback. Both are
  permitted unprivileged in a container — lowering your own priority never needs
  `CAP_SYS_NICE`.
- **`TCP_NODELAY` on.** Nagle would add tens of milliseconds to small responses.
- **The hash loop is already allocation-free.** The Go and Node starters do
  ~100k allocations per request; that win is banked.

### Tuning knobs

| Env var | Default | Notes |
|---|---|---|
| `PORT` | `8080` | Required by the grader |
| `IO_THREADS` | `2` | epoll loops |
| `RISK_WORKERS` | `2` | Hash workers, `SCHED_IDLE` |
| `RISK_QUEUE` | `512` | Queue full ⇒ 503. Deep on purpose |
| `RISK_DEADLINE_MS` | `0` | `0` disables. Sheds jobs past their budget |

Queue depth is deliberately deep: filling it spends error budget. If it fills
under graded load, that is a signal to investigate, not a knob to raise.

---

## 5. Measured baseline

Single core, Apple M-series (ARM crypto extensions), `-O3`. **Not** the grading
hardware, and not the Spark.

| SHA-256 backend | `/risk` per request | Implied 2-core mix ceiling |
|---|---|---|
| Portable scalar | 17.67 ms | ~1,130 req/s |
| **OpenSSL (hardware)** | **9.10 ms** | **~2,200 req/s** |

**1.94× from hardware SHA alone.** This is the whole thesis in one measurement,
and why OpenSSL is the default backend.

### Verification status

Verified: the correctness gate passes on **both** backends, 11/11, identical
digests — FIPS 180-4 vectors, short chains, and the full 50,000-round result for
fixed seeds. `/stats` matches a Python reference to ~5e-14 (float summation
order; the grader only checks status 200). `/price` renders exactly
`{"symbol":"AAPL","price":187.42}`.

**Not verified:** the image has never been built and the server has never run —
no Docker or Linux was available on the dev machine. `http_server.cpp`,
`risk_pool.cpp`, and `main.cpp` are `-Wall -Wextra` clean under a syntax and
type check against stubbed Linux headers, which catches C++ mistakes but **not**
epoll semantics, the deferred-completion handoff, or keep-alive under real
connections. Treat those three as unproven until they run.

Also unverified: the **pinned apt versions** in the Dockerfile are a best guess
at what `debian:bookworm-20240926-slim` ships. If the build fails on a version
mismatch, run `apt-cache policy g++ cmake libssl-dev` in the base image and
correct them. Do not drop the pins — reproducible builds are a track rule.

---

## 6. Which machine for which work

Two machines are in play: this Apple Silicon Mac, and the DGX Spark. They are
good at different halves of the problem, and the split is not the obvious one.

| Work | Machine | Needs Docker? |
|---|---|---|
| Hash loop optimisation + microbenchmarks | **Mac** | No |
| Correctness selftest | **Mac** | No |
| Compiling, syntax checking, editing | Either | No |
| Latency, tail behaviour, concurrency | **Spark** | Yes |
| Anything under the 2-CPU cap | **Spark** | Yes |
| k6 runs of any kind | **Spark** | Yes |
| Write-up and pitch | Either | No |

### Why the Mac cannot give trustworthy latency numbers

Docker Desktop on Apple Silicon runs containers inside a **Linux VM**, and that
VM sits directly on top of the three things this track measures:

- **CPU accounting.** The challenge is about cgroup CFS quota behaviour under
  saturation — bursting, throttling, stall periods. In a VM, `--cpus=2` is a
  quota against cores the hypervisor is itself scheduling: a quota inside a
  quota. Even the core-count gotcha misbehaves, since `nproc` reports the VM's
  allocation rather than the host's core count, so the trap the track is built
  around does not reproduce faithfully.
- **Network path.** Published ports on macOS traverse a userspace proxy at the
  VM boundary. The graded metric is `/price` p95 *in milliseconds*, so a
  meaningful share of what you would be measuring is the proxy. It is entirely
  possible to spend a day optimising an artefact of Docker Desktop networking.
- **`SCHED_IDLE`.** The most important architectural decision in this build is
  that hash workers run at the weakest scheduler priority so the kernel
  preempts them instantly. That is real Linux scheduler behaviour and it should
  be verified on real Linux.

The Spark has none of these: native cgroups, native networking, native
scheduler, and ~20 cores so k6 never competes with the container. `--cpus` and
`--cpuset-cpus` behave exactly as the grader's will.

### What the Mac is genuinely good for

The thing the Mac is best at does not need Docker at all.

The hash loop is pure computation — no container, no sockets, no cgroups. The
baseline in section 5 was measured natively in about two seconds:

```
clang++ -std=c++17 -O3 -I src src/sha256.cpp src/risk.cpp src/data.cpp bench.cpp -o bench
```

For the optimisation that actually wins the track, this gives a **cleaner**
signal than a container would, because there is no quota or VM noise in the
path. It is also arm64 with crypto extensions, the same family as the Spark, so
the *ratios* transfer even though the absolutes will not. Same for the selftest:
11/11 natively, no Docker involved.

So the fast iteration loop is: optimise and microbenchmark the chain on the Mac,
then take the winning version to the Spark and confirm it under load.

### Is installing Docker on the Mac worth it?

Marginally, for one narrow purpose: **confirming the image still builds** when
away from the Spark — particularly the apt version pins, which are a guess and
the likely first failure. That is build validation, not performance, and the VM
does not distort it.

Do not run k6 through it and believe the output. If running it locally anyway,
at least run k6 *inside* a container on the same Docker network to skip the
port-forward proxy; the numbers will still be soft, just less wrong.

### The third machine worth considering

Neither machine is x86. If the organisers confirm the grading box is x86, a
**cheap 2-core x86 cloud VM** becomes worth an hour: it is the only way to get
numbers on the real architecture, and the only place AVX2 multi-buffer SHA-256
can be developed or tested at all. A few dollars, and it settles the largest
open question in the roadmap.

Gated, as ever, on asking the organisers what they grade on.

---

## 7. Moving to the DGX Spark

The Spark has Docker, which unblocks everything above. It also changes two
things that matter.

### It is ARM64, not x86

Confirm first:

```
uname -m          # expect aarch64
lscpu             # core topology, and check for the sha2 feature flag
nproc
```

Consequences:

- **AVX2 multi-buffer SHA-256 is off the table on this machine.** It is x86
  only. ARM has the single-lane `sha256h`/`sha256h2`/`sha256su0`/`sha256su1`
  instructions, which OpenSSL already uses — so the easy win is banked, but the
  biggest remaining swing is not available to develop or test here.
- **You do not know the grading box's architecture.** So: never `-march=native`,
  always runtime CPU dispatch, always keep the portable fallback. Any
  ARM-specific assembly you write must not be the only path. The CMakeLists
  already refuses `-march=native` for this reason.
- If you want to pursue multi-buffer, you need an x86 machine to develop it on,
  and you would be betting the grading box is x86. Probably not worth it unless
  the organisers confirm the architecture. **Ask them.** That single question
  changes the whole optimisation roadmap.

### Heterogeneous cores are a measurement hazard

The GB10 CPU mixes performance and efficiency cores. Whether your container
lands on fast or slow cores will swing your numbers substantially run to run,
which makes unpinned benchmarking nearly useless.

So pin deliberately. Check the topology with `lscpu -e` first, then:

```
# Container: 2 cores, pinned, matching the grader's caps
docker run --rm --cpus=2 --memory=2g --cpuset-cpus=0,1 \
  -p 8080:8080 obsidio-cpp

# k6: on completely different cores, so it never competes
taskset -c 8-15 k6 run -e TARGET=http://127.0.0.1:8080 k6/grading.js
```

Two caveats worth knowing:

- `--cpus=2` is a **CFS quota**, not a pinning. The grader runs it *unpinned*, so
  on a 20-core box the container's threads can spread across many cores while
  sharing 2 cores' worth of quota. That has a different tail-latency profile
  than 2 dedicated cores. Do your day-to-day tuning pinned (repeatable), then
  **sanity-check with plain `--cpus=2` unpinned**, because that is literally
  what the grader runs.
- With ~20 cores you have plenty of room to keep k6 off the container entirely.
  Do it — same-box contention produces numbers you will make bad decisions on.

### The GPU is irrelevant

Do not be tempted. The chain is sequential, so a single request cannot use a
GPU meaningfully; the container is CPU-capped; and the grading box almost
certainly has no GPU at all. Any GPU work is unscoreable.

### Absolute numbers will flatter you

Cortex-X925 cores are fast. Expect better `/risk` timings on the Spark than the
grading box will give you. **Optimise on ratios and margins, not absolutes**, and
leave headroom against the thresholds — which are placeholders pending
recalibration on grading hardware anyway.

---

## 8. First session on the Spark

```
cd starters/cpp
docker build -t obsidio-cpp .        # the selftest gates the build
docker run --rm --cpus=2 --memory=2g -p 8080:8080 obsidio-cpp
curl http://127.0.0.1:8080/health
```

Then, in order:

1. **Fix whatever the build surfaces** (apt pins are the likely first failure).
2. **Prove the server works** — hit all four endpoints by hand, confirm
   keep-alive, confirm a `/risk` digest matches the Python reference.
3. **Run the sanitised build once** and put load through it:
   ```
   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOBSIDIO_SANITIZE=ON
   cmake --build build-asan -j && ./build-asan/obsidio-server
   ```
   ASan and UBSan turn silent corruption into a loud stack trace. This is what
   makes C++ survivable on a deadline. Never ship this build; it is ~2× slower.
4. **Baseline k6 run.** Write the numbers down. This is measurement zero.
5. **Only then optimise.**

---

## 9. Optimisation roadmap

In `src/risk.cpp`, by expected payoff:

1. **Specialised two-block transform.** Input is always exactly 64 bytes, so
   every hash is two blocks and the second is a compile-time constant (`0x80`,
   zeros, length = 512 bits). Precompute its message schedule, skip the generic
   padding path.
2. **Crypto intrinsics directly** — ARMv8 `vsha256hq_u32` etc. on the Spark,
   SHA-NI on x86 — with runtime dispatch and the portable fallback kept.
3. **SIMD hex encoding** — 32 bytes to 64 chars in a few instructions.
4. **Multi-buffer SHA-256** — 8 chains in lockstep. Biggest win, biggest cost,
   **x86 only**. Gated on knowing the grading architecture.

Then, separately, the persistence bonus: append-only file on a mounted volume,
replayed at startup. Cheap, and it is real points.

### Do not break the digest

`tests/selftest.cpp` runs as a **build step**. Keep it that way. A wrong digest
still returns a plausible 64-char hex string, the grader verifies it against the
seed, and that request scores **zero**. Every optimisation above is a chance to
silently break the chain.

Regenerate any golden value with:

```
python3 -c "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
```

---

## 10. The half most teams skip

The write-up and video are required, and the on-theme prize explicitly rewards
*deliberate* engineering over luck.

**Keep a numbers log from measurement zero.** Every change: what you predicted,
what you measured before and after, whether you were right. That log *is* the
resilience write-up and the video script. It costs ten minutes a day and it is
the difference between "it held" and "here are the four numbers that prove I
knew why."

Things worth being able to say out loud, all of which are already true of this
build and none of which are obvious:

- Score is proportional to throughput, and throughput is bounded almost entirely
  by one hash loop — so that is where the effort went.
- The 1% error ceiling means shedding is not a real lever; admission control is.
- `SCHED_IDLE` on the hash workers solves head-of-line blocking at the kernel
  rather than in application code.
- Persistence is nearly free because `POST /price` is not in the load mix.
- Hardware SHA is worth 1.94× on its own, measured.

---

## 11. Open questions

- **What architecture is the grading box?** Ask the organisers. It decides
  whether multi-buffer SHA-256 is worth pursuing.
- **Final threshold values.** Current ones are placeholders pending calibration.
  Build for margin, do not over-fit to 200 ms.
- Does the grader restart the container with `docker restart` (same filesystem)
  or `rm` + `run` (needs a volume)? Assume the stricter case: use a volume.

---

## 12. x86 SHA-NI back end: written blind, then verified on real hardware

`src/chain_x86.cpp`: the same two-block specialisation as `chain_arm.cpp`
(real schedule for block 1, precomputed constant schedule for block 2), built
on the x86-64 SHA extensions (`sha256rnds2`/`msg1`/`msg2`) via intrinsics,
with runtime CPUID dispatch (`__builtin_cpu_supports("sha")`) and a
portable-path fallback exactly like the ARM back end.

It was written with no x86 machine with a C++ toolchain available — every
instruction sequence was derived by hand from each intrinsic's documented
semantics (the permute/state-layout dance in particular was independently
re-derived lane-by-lane against `_mm_sha256rnds2_epu32`'s documented
(C,D,G,H)/(A,B,E,F) contract) rather than copied on faith, but it shipped
initially gated behind `RISK_BACKEND=x86-sha-ni` rather than trusted, because
this codebase's own rule (`tests/selftest.cpp`, chain_arm.cpp's comments) is
that an accelerated back end doesn't auto-select until it clears
`verify_backend()` on its real target — and the Dockerfile runs that selftest
as a build step, so a wrong auto-selected back end would fail the **entire
Docker build**, not just run slow.

**Verified 2026-08-22.** WSL2 turned out to already be installed on the dev
machine (Ubuntu 24.04, `g++` 13.3.0, `cmake` already present), and the CPU —
an AMD Ryzen 7 6800HS — advertises `sha_ni` in `/proc/cpuinfo`, so this was
testable immediately instead of waiting on a cloud VM:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest
```

All 21 digest checks passed: FIPS 180-4 vectors, short chains, both full
50,000-round chains, and every x2-interleaved variant (including swapped
lanes and the n=1 edge case). A standalone wall-clock A/B (`bench/
bench_time.cpp`, same binary, same run, forcing `RISK_BACKEND` per process)
gave, across three repeated trials each:

| Back end | ms/chain | chains/s/core |
|---|---:|---:|
| `reference` (OpenSSL per-call) | 16.6-17.0 | ~60 |
| `x86-sha-ni` | 4.20-4.23 | ~238 |

**~4.0x**, bigger than the ~1.5-3x this section originally guessed as the
likely incremental win over an already-OpenSSL-accelerated baseline. It has
been promoted to auto-selected in `select_backend()` (`src/risk.cpp`), the
same as the ARM back end — the `RISK_BACKEND=x86-sha-ni` opt-in gate has been
removed now that it has cleared the same bar the ARM path did.

**Caveat worth keeping:** WSL2 is a real Linux kernel with real cgroup and
CPUID behaviour, not a translated/emulated environment like Docker Desktop's
old Hyper-V path, so this number should transfer reasonably well — but it is
still not the grading box, and not even a Docker container (no `--cpus=2`
quota was in effect for this measurement, just a raw process). Confirm the
ratio holds once the image is actually built and run under
`--cpus=2 --memory=2g`, the way section 8 describes for the Spark.
