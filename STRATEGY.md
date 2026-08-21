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

This model held all the way through, and is now good enough to forecast an
optimisation before writing it. At x4 the effective per-chain cost is ~1.5 ms,
predicting ~13,300 req/s against **13,140 measured**; `score ≈ 2.5 × requests`
predicted 32,850 points/s against **32,838 measured**. Note that "cost of the
chain" means *amortised* cost — four chains interleaved take ~6.1 ms wall time
and return four answers. The lever turned out to be throughput per chain, not
latency per chain, and those are not the same number.

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
2. **Multi-buffer SHA-256** — several chains in lockstep. This was written down
   as "AVX2, x86 only". That turned out to be **wrong**, and it is the single
   most valuable thing this section got backwards: the same idea works on ARM
   using the NEON register file, and it is now shipped and worth **+36%** on its
   own. See section 5. The x86 architecture question is correspondingly less
   urgent than section 11 makes it sound.
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
src/chain_backend.hpp  Pluggable chain back end: chain1/chain2/chain3/chain4.
src/chain_arm.cpp      ARMv8 crypto-extension chain. Where the score comes from.
src/risk.cpp           Back-end selection + self-verification. The 50,000 rounds.
src/data.cpp           Symbols, 500-point series, JSON rendering.
src/http_server.cpp    Dependency-free epoll HTTP/1.1 server.
src/risk_pool.cpp      Bounded queue, SCHED_IDLE workers, x4..x1 lane batching.
src/main.cpp           Handlers and wiring.
tests/selftest.cpp     Correctness gate, 56 checks. Runs during docker build.
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
- **The hash loop is allocation-free.** The Go and Node starters do ~100k
  allocations per request; that win is banked.
- **Four chains run interleaved in one worker thread.** This is where the score
  actually comes from and it is the least obvious thing in the build, so it is
  worth saying precisely: `sha256h` has ~4-cycle latency but ~1-cycle
  throughput, so one serial chain leaves most of the crypto pipeline idle
  waiting on its own dependency. Four *independent* chains advanced in lockstep
  ride along in those bubbles. This is instruction-level parallelism inside a
  single thread — **not** extra threads, not extra cores. `RISK_WORKERS` is
  still 2 and the container still sits at exactly its 2-CPU quota.
- **The back end verifies itself before it is selected.** `verify_backend()` in
  `risk.cpp` reproduces reference digests for every lane count against five
  seeds and six chain lengths at startup. A back end that fails is rejected and
  the reference path runs instead, and the self-test turns rejection into a
  build failure. A wrong digest is worth zero and looks exactly like a right
  one, so this gate is the difference between fast and fast-and-scoring.
- **`-march=armv8-a+crypto` is scoped to `chain_arm.cpp` alone**, never the
  target and never `-march=native`. Selection is gated on `HWCAP_SHA2` at
  runtime, so the binary still starts on a CPU without the extension.

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

## 5. Measured results

All figures below are measured on the Spark (aarch64, 20 cores, `sha2` present)
under the real caps, `--cpus=2 --memory=2g`, container **unpinned** exactly as
the grader runs it, k6 on separate cores so it never competes. Still not the
grading hardware — optimise on ratios, not absolutes.

### Where the score went

| Build | `work_score` | vs previous | `/risk` p95 | req/s |
|---|---|---|---|---|
| OpenSSL one-shot `SHA256()` | 2,997,122 | — | 399 ms | 4,439 |
| ARMv8 crypto, x2 interleaved | 6,508,268 | **+117%** | 184 ms | 9,650 |
| ARMv8 crypto, x3 interleaved | 8,074,407 | **+24.1%** | 146 ms | 11,957 |
| **ARMv8 crypto, x4 interleaved** | **8,866,401** | **+9.8%** | **132 ms** | **13,140** |
| x5 interleaved (tested, dropped) | 8,922,428 | +0.63% | 130 ms | 13,217 |

**2.96× end to end.** Every row cleared all four thresholds with 0.00% errors.

Two findings worth carrying into the pitch, because neither is obvious:

**Hand-written intrinsics for a single chain are worth ~3%.** OpenSSL's assembly
is already at the metal; `armv8 x1` measured 3.38 ms/chain against OpenSSL's
3.48. Essentially the entire win is the *interleave*, not the instructions.
Anyone who writes SHA intrinsics and stops there has done the hard work and
collected almost none of the payoff.

**The isolated chain benchmark understates the interleave, twice.** It predicted
x3 ≈ +10% (delivered +24%) and x4 ≈ 0% (delivered +9.8%). The gap is batching,
not hashing: with a smaller batch size a worker that finds an awkward queue
depth runs the remainder through a shorter, slower path. A microbenchmark of the
kernel cannot see that, so trust the end-to-end number when they disagree.

### Why four lanes and not more

Lane scaling delivered +24.1%, +9.8%, +0.63%. A `Lane` is six 128-bit vectors,
so four lanes hold 24 of the 32 NEON registers and five want 30 — leaving
nothing for round scratch, and spill starts cancelling the batching win exactly
where the register file says it should. x5 was built and measured rather than
argued about: its gain is real but small, and it costs a fifth verification
seed, another 16 µs on the fast path, and needs five queued jobs to reach its
best path, so it degrades further at low load. Four is the call.

The cost of each lane is consistent: `/price` p95 climbs ~22 µs per lane
(461 → 485 → 511 → 527 µs across x2..x5) because a worker commits to more chains
before it yields. At x4 that is still ~390× under the bar. Latency is nowhere
near binding — but it is the number that binds first, not memory and not cores.

### Reproducibility

Two runs of the committed x4 tree scored 8,866,401 and 8,859,797 — a spread of
**0.075%**. That is tight enough to trust a 1% difference, which is why x5's
+0.63% is reported above as a real-but-small gain rather than dismissed as
noise.

### Verification status

**Everything the earlier draft of this section listed as unproven is now
verified on real Linux under the real caps.**

- Correctness gate: **56/56**, and it runs as a build step inside the image.
  FIPS 180-4 vectors, per-lane digests, rotated seed order, all-same-seed, short
  chains, and cross-checks that x4 agrees with x3/x2/x1 on shared lanes — without
  that last one the served digest could depend on how deep the queue happened
  to be.
- The 50,000-round digest for `seed=0.5` matches the Python reference exactly,
  natively and out of the running container.
- The image builds; the server runs under `--cpus=2 --memory=2g` and serves all
  four endpoints. epoll, the deferred `/risk` completion handoff, and keep-alive
  across one connection are confirmed — those were the three files flagged as
  unproven.
- Clean under **ASan + UBSan**.
- Peak container memory under full load: **6.1 MiB of 2 GiB** (0.3%). The lanes
  live in registers. CPU sits at ~204%, i.e. exactly the 2-core quota, never
  above it.

The **apt version pins** were indeed a wrong guess and are fixed: `libssl-dev`
and `libssl3` are `3.0.20-1~deb12u2` in `debian:bookworm-20240926-slim`, not
`3.0.17`. Re-check with `apt-cache policy g++ cmake libssl-dev` in the base
image if it breaks again — bookworm security updates move these. Do not drop the
pins; reproducible builds are a track rule.

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

- ~~**AVX2 multi-buffer SHA-256 is off the table on this machine.**~~ **This was
  wrong, and expensively so.** AVX2 specifically is x86-only, but *multi-buffer
  as an idea* is not: ARM's `sha256h`/`sha256h2`/`sha256su0`/`sha256su1` are
  single-lane instructions, yet nothing stops you running four independent
  chains through them in one loop and letting the NEON register file hold four
  lanes of state. That is exactly what `chain_arm.cpp` does, and it is worth
  +36% — the biggest win on the board, available here all along. The lesson
  generalises: "the vendor's named multi-buffer extension is x86-only" is not
  the same claim as "instruction-level parallelism is x86-only".
- **You do not know the grading box's architecture.** So: never `-march=native`,
  always runtime CPU dispatch, always keep the portable fallback. Any
  ARM-specific assembly you write must not be the only path. The CMakeLists
  already refuses `-march=native` for this reason.
- An x86 SHA-NI back end is still worth building *if* the organisers confirm the
  grading box is x86 — `chain_backend.hpp` has the seam for it. But it is no
  longer the blocking question it looks like here, because the ARM path already
  collected the multi-buffer win. **Ask them anyway.** That single question
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

## 8. Running it

The first-session checklist is done — build fixed, server proven by hand,
digest checked against the Python reference, ASan clean. This is now just how
you reproduce a number.

```
cd starters/cpp
docker build -t obsidio-cpp .        # the 56-check selftest gates the build
docker run -d --name obsidio-test --cpus=2 --memory=2g -p 8098:8080 obsidio-cpp
curl http://127.0.0.1:8098/health
taskset -c 5,6,7,8,9 k6 run -e TARGET=http://127.0.0.1:8098 ../../k6/grading.js
```

k6 is v2.2.0, linux/arm64. Keep it off the container's cores — same-box
contention produces numbers you will make bad decisions on. The container is
left **unpinned** on purpose: that is what the grader does, and with a 2-core
quota spread over 20 cores it costs only ~4.7% against pinning to two
performance cores, so there is no reason to flatter the number.

Sanitised build, worth running after any change to the chain:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOBSIDIO_SANITIZE=ON
cmake --build build-asan -j && ./build-asan/obsidio-selftest
```

Never ship it; roughly 2× slower.

**Watch the core placement if you ever pin.** The GB10 is heterogeneous — CPUs
0–4 and 10–14 run at 2.8 GHz, 5–9 and 15–19 at 3.9 GHz. Same binary, 26.4 ms
vs 16.6 ms per chain depending purely on where it landed. Check `lscpu -e`
first.

---

## 9. Optimisation roadmap

In `src/risk.cpp`, by expected payoff:

Almost all of the original list is **done**, and it landed in a different order
of importance than predicted:

1. ~~**Specialised two-block transform.**~~ Done. The second block of a 64-byte
   message is a compile-time constant, and its message schedule is precomputed.
2. ~~**Crypto intrinsics directly.**~~ Done, `chain_arm.cpp`, runtime-gated on
   `HWCAP_SHA2` with the portable path kept. Worth only ~3% by itself — see
   section 5.
3. ~~**SIMD hex encoding.**~~ Done — `vqtbl1q_u8` writes the digest straight into
   the next round's message vectors, no store and reload.
4. ~~**Multi-buffer SHA-256.**~~ Done, and it is **not** x86-only as this list
   originally claimed. Four chains interleaved on ARM, +36% over x2. The
   biggest single win on the board, exactly as predicted — just on the wrong
   architecture.

What is actually left, in order:

1. **The persistence bonus.** Append-only file on a mounted volume, replayed at
   startup. `POST /price` is not in the k6 mix, so it costs ~nothing during the
   graded run. This is now the largest uncollected item on the board — real
   points for an afternoon, versus low single digits from more chain work.
2. **x86 SHA-NI back end.** Only worth building if the organisers confirm the
   grading box is x86. `chain_backend.hpp` already has the seam: write the four
   `chain*` functions and return them from a `sha_ni_backend()`.
3. **More lanes.** Measured and rejected — x5 gives +0.63% for real cost. Do not
   revisit without new evidence.
4. **`/price` and `/stats` fast-path work.** Currently ~500 µs p95 against a
   200 ms bar. There is nothing to win here; do not be tempted.

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
  by one hash loop — so that is where the effort went. 2.96× end to end.
- The 1% error ceiling means shedding is not a real lever; admission control is.
- `SCHED_IDLE` on the hash workers solves head-of-line blocking at the kernel
  rather than in application code. The proof is one pair of numbers: under
  saturation `/price` p95 is ~500 µs while `/risk` p95 is 132 ms — three orders
  of magnitude apart, *at the same moment*.
- Persistence is nearly free because `POST /price` is not in the load mix.
- **Writing SHA-256 intrinsics by hand was worth 3%.** Interleaving four
  independent chains through them was worth +36%. The instruction was never the
  bottleneck; the dependency chain was. `sha256h` has ~4-cycle latency and
  ~1-cycle throughput, so a serial chain leaves the pipeline mostly idle.
- That win needed **no extra threads and no extra cores** — it is ILP inside one
  worker, on the same 2-CPU cap, using 0.3% of the memory budget.
- The lane count was chosen by measurement, not taste: +24.1%, +9.8%, +0.63%,
  stopping exactly where the NEON register file runs out. x5 was built, measured
  and thrown away.
- The isolated microbenchmark was wrong twice, both times understating the
  interleave, because it cannot see queue batching. End-to-end wins the tie.

---

## 11. Open questions

- **What architecture is the grading box?** Ask the organisers. Now a smaller
  question than it was: the multi-buffer win is already collected on ARM, so
  this only decides whether an x86 SHA-NI back end is worth adding, not whether
  the main idea works. If the box is x86, the ARM back end simply does not
  qualify at runtime and the OpenSSL path serves — correct, but ~3× slower, so
  it is worth knowing.
- **Final threshold values.** Current ones are placeholders pending calibration.
  Build for margin, do not over-fit to 200 ms.
- Does the grader restart the container with `docker restart` (same filesystem)
  or `rm` + `run` (needs a volume)? Assume the stricter case: use a volume.
