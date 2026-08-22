# Obsidio: hitting the theoretical limit on the amd64 box

Everything needed to go from today's ~4.2M work_score to the ~8.4M ceiling on
the grading machine. Written after three full passes over the code and docs;
the ceiling arithmetic, the implementation plan for `chain_x86.cpp`, every
residual scheduling win, and the things deliberately left alone.

Companion to `STRATEGY.md`, which holds the history and the ARM measurements.
This file is the forward plan.

---

## 1. The number

Score ≈ 2.5 × requests served. Run length = 270 s (1m ramp + 2m climb + 1m
peak + 30s cooldown). The closed-loop VU model means offered load equals our
own throughput whenever VUs ≥ ~50, so the run is effectively saturated end to
end — confirmed by the ARM numbers: 13,140 req/s × 270 s × 2.5 ≈ 8.87M against
8,866,401 measured.

On Zen (`sha256rnds2`: latency 4, reciprocal throughput 2; two blocks per hash,
so 64 rounds per hash):

| Back end | Cycles/hash | risk_ms @4 GHz sustained | req/s | work_score / run |
|---|---|---|---|---|
| Serial OpenSSL (**today**) | 256 | ~3.2 | ~6,250 | **~4.2M** |
| SHA-NI x2 interleaved | 128 | ~1.6 | ~12,500 | **~8.4M** |

**The entire remaining gap is one file: `src/chain_x86.cpp`.** Everything else
in this document sums to low single digits. If only one thing gets done before
grading, it is this file.

Honesty clause: 8.4M assumes ~4 GHz sustained. A Ryzen 6000 mobile part under a
five-minute saturating siege is a thermal event; if it settles at 3.5 GHz the
ceiling moves to ~7.3M. Nothing can be done about that except measuring the
real sustained clock first and knowing the true number before the judges do.

---

## 2. `src/chain_x86.cpp` — the implementation

Mirror `chain_arm.cpp` exactly in shape. Same contract, same verification, new
instructions.

### 2.1 The instruction set

```
_mm_sha256rnds2_epu32   one round pair; takes (state_efgh, abef, msg) + imm
_mm_sha256msg1_epu32    message schedule, partial
_mm_sha256msg2_epu32    message schedule, final 4 words
_mm_shuffle_epi8        byte-lane shuffle — used for hex encoding
_mm_shuffle_epi32       used for ABEF/CDGH packing
```

Compile with `-msse4.1 -msha` **scoped to this translation unit only**, exactly
as `-march=armv8-a+crypto` is scoped to `chain_arm.cpp`. Never the target,
never `-march=native`. Runtime gate:

```cpp
__builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1")
```

returning `nullptr` when either fails, so the binary still boots anywhere.

### 2.2 The state-layout trap

SHA-256's natural order is a..h. Intel's SHA-NI keeps state split as two
registers: **ABEF** and **CDGH**. Every hash needs:

- a prologue shuffle packing `{a,e,i,m}` → ABEF and `{c,g,k,o}` → CDGH
  (`_mm_shuffle_epi32` with `0xB1` / `0x1B` patterns),
- the rounds,
- an epilogue unpack back to a..h.

Get either shuffle wrong and the digest is wrong — which is why the next point
exists.

### 2.3 Correctness is already guarded — lean on it

`verify_backend()` in `risk.cpp` reproduces reference digests for every lane
count across five seeds and six chain lengths at startup, and the selftest runs
as a Docker build step (twice — once accelerated, once forced-reference).
A wrong digest scores zero and looks exactly like a right one; this gate is the
difference between fast and fast-and-scoring. Extend the seed/length matrix if
it does not already cover the x86 lane counts.

### 2.4 The two structural wins to carry over from ARM

**Two-block transform with a constant second block.** After round one the input
is always exactly 64 lowercase hex chars, so each hash is block 1 (message =
hex chars) plus block 2 (padding + length — fully known at compile time).
Precompute block 2's message schedule once. This was worth 16.62 → 4.74
ms/chain on its own against naive OpenSSL EVP calls.

**Hex encode fused into the schedule.** On ARM, `vqtbl1q_u8` writes the digest
straight into the next round's message vectors with no store-and-reload. On
x86, `_mm_shuffle_epi8` with a 16-byte nibble→ASCII LUT does the same. Do not
hex-encode to memory and re-read.

### 2.5 The interleave — where the score actually lives

One serial chain leaves the SHA unit idle waiting on its own latency (4 cycles
per rnds2 vs throughput 2). Two independent chains advanced in lockstep fill
those bubbles: predicted 128 cycles/hash amortised vs 256 serial. This is ILP
inside one worker thread — no extra threads, no extra cores, quota untouched.

Build `chain1` and `chain2` first. `chain3`/`chain4` can exist behind the same
interface but expect them to lose on Zen (see §4.1).

Register budget check: a lane needs roughly 6 XMM registers; Zen has 16
architectural. Two lanes ≈ 12, comfortable. Four would want ~24 — spill starts
exactly where the register file says it should.

### 2.6 Round zero stays portable

After the first hash the value is always 64 hex chars, but round zero hashes an
arbitrary-length seed. Keep round zero on the generic path (`sha256_64` /
OpenSSL one-shot) and hand off to the backend from round one onward — same
split as ARM. Cost: one generic hash per 50,001 — noise.

---

## 3. Closing the last few percent (measured → theoretical)

Even with x2 running perfectly, these are what separate a measured ~12,000 req/s
from the clean-room 12,500:

### 3.1 Thread configuration: flip to io=1 / rw=2 (after re-sweeping)

The sweep on the Spark put io=1/rw=2 at +3.9% over the committed io=2/rw=2,
confirmed across reruns (0.24% spread). One epoll loop is plenty for
microsecond fast-path work; the freed scheduling headroom feeds the hash
workers. Re-sweep on amd64 before landing — SMT siblings under a 2-CPU quota
schedule differently than Spark cores did.

### 3.2 Batch-collect wait in `risk_pool.cpp`

Workers currently drain greedily. During the ~100 s of ramp phases, queues run
shallow and workers fall off the x2 path onto slow serial chains. A small
adaptive wait (~1–5 ms) before pickup lets batches fill to 2+ lanes. Against
the 1500 ms `/risk` bar the added latency is invisible; under saturation it
changes nothing (queue always deep). Ramp phases are a third of the run — this
is worth real points at the edges. Measure end-to-end, never in the isolated
benchmark (the microbenchmark understated the interleave twice).

### 3.3 SMT-aware affinity

If both hash workers land on hyperthread siblings of one physical core, they
share a single SHA unit and the whole win halves. Read topology at runtime from
`/sys/devices/system/cpu/cpu*/topology/thread_siblings_list`, then
`sched_setaffinity` the IO thread(s) and each worker onto distinct physical
cores. Unprivileged, legal inside the container. Log the placement at startup
for debugging on unknown hardware.

### 3.4 Pre-warm before binding the listener

work_score counts every served request including the warm-up minute. After
`init_risk_backend()`, push dummy jobs through the pool so caches, branch
predictors and page tables are hot before request one.

### 3.5 Build flags

- Add `-flto` to Release (`CMakeLists.txt:16`) — cross-TU inlining through the
  backend dispatch seam.
- PGO (`-fprofile-generate/-fprofile-use`) trained on a k6-shaped workload —
  stacks with LTO, low single digits.
- Optional while waiting: clang-vs-gcc A/B on the hot TU; intrinsic scheduling
  differs between compilers.

Expected total from §3: roughly +5–10% over a bare x2 port. Real, but do not
start here — §2 first, always.

---

## 4. Measuring on the real architecture

### 4.1 The written-down predictions (made before measuring, on purpose)

| Experiment | Prediction |
|---|---|
| Lane sweep x1 vs x2 | **x2 wins ~2×** |
| x3 | wash or tiny gain |
| x4 | negative (register spill + ratio 2) |
| io=1/rw=2 vs io=2/rw=2 | io=1 wins, smaller margin than ARM's +3.9% |
| Batch-wait on/off | on wins during ramps, neutral at peak |

Section 5 of STRATEGY.md records that the isolated microbenchmark mispredicted
the ARM interleave twice, both times low. End-to-end k6 runs win every tie.

### 4.2 Where to measure

Best first: any cloud VM with Zen-family silicon — AWS `c6a`/`m6a` (EPYC
Milan), Hetzner `CCX`. Same `sha256rnds2` timings, same 16-register file.
Better: bare-metal Ryzen (Hetzner `AX`, or the actual grading-class hardware if
accessible). Since grading is reportedly a **Windows host**: rehearse under
Docker Desktop / WSL2 on any local AMD Windows machine — that reproduces the
hypervisor scheduler and NAT layer Linux rehearsals miss.

Emulated amd64 (qemu/buildx) is for **correctness only**. Timings are
meaningless and SHA-NI may be absent entirely, silently testing the fallback.

### 4.3 Windows-host checklist

- Confirm inside the container: `lscpu` shows `sha_ni`, correct core count.
- Check WSL2 topology passthrough for the affinity code (§3.3) — sibling lists
  must resolve or the code must degrade gracefully.
- Plug in, high-performance power plan; laptop parts throttle on battery.
- Run back-to-back full ramps and record decay run-over-run — quantify the
  thermal drift before the judges discover it.
- Per-second throughput curves, not just k6 summaries: exposes CFS-throttle
  periodicity (100 ms periods) and thermal sag within a run.

---

## 5. Finish the persistence bonus (it is NOT actually done)

`STRATEGY.md` §9 claims it complete, but the code disagrees: no
`src/persist.cpp`, no `tests/persist_test.sh`, POST /price is in-memory only
(`main.cpp:197-200`), and `compose/docker-compose.yml` is still the untouched
Node+Postgres template. Either lost work or a doc ahead of reality — either way
the points are unclaimed.

Design (unchanged, because it is right): append-only log, one atomic
`O_APPEND` write + `fdatasync` per POST, replayed at boot, named volume via
compose. POST /price is absent from the graded mix, so the cost during the run
is ~zero. No Postgres — that would spend half the CPU budget for nothing.

Hardening details:

- **Rewrite the compose file for a single cpp service + named volume**, keeping
  the *entire* 2-CPU budget for the app. Submitting the current template costs
  0.5 CPU for a database that does nothing.
- Test survival under `docker kill -9`, not just graceful stop — the grader
  restarts mid-run and SIGKILL is the stricter case.
- Cap/compact the log on clean replay.
- Keep startup fast: replay then bind, since downtime during the restart eats
  served requests and error budget.

---

## 6. Do-not-touch list (verified dead ends)

- **io_uring / syscall optimization** — IO path is ~2% of compute.
- **malloc tuning** — hot loop allocation-free; response strings are noise.
- **Quota-edge tail smoothing** — worst-case CFS stall 100 ms vs `/price` p95
  headroom of 400×.
- **Fast-path JSON/stats work** — ~500 µs p95 against a 200 ms bar.
- **More threads/cores/GPU** — quota fixed; GPU unscoreable.
- **Load shedding beyond ~1%** — the error ceiling makes rejection a worse
  trade than queueing. Deadline shedding stays disabled.
- **Math shortcuts on the chain** — closed. Length-extension does not apply
  across rounds; hex-encoding cannot be skipped (spec-verified); the constant
  second block is already precomputed.

---

## 7. Execution order

1. **Write `src/chain_x86.cpp`** (+ CMake gate, verify matrix, selftest x86
   branch in the Dockerfile's double-run).
2. **Correctness on amd64** — emulated build is fine for "right digest".
3. **Get real Zen access; lane + thread sweeps; land defaults.**
4. **§3 residuals in payoff order:** affinity, batch-wait, io flip, LTO/PGO,
   pre-warm.
5. **Persistence bonus build-out + compose rewrite + kill -9 drill.**
6. **Dress rehearsal, timed end-to-end, twice:** fresh clone → build → run →
   full k6 → restart-under-load → teardown, under the closest-to-grading
   environment available.
7. **Re-run everything when the final `grading.js` lands** — thresholds and
   ramp are placeholders until locked.

Keep feeding the numbers log in `STRATEGY.md`. Predicted-before-measured,
measured-after, honest deltas. That log is the resilience write-up and the
video script; the on-theme prize rewards deliberate engineering, and the log is
the proof of deliberateness.

---

*Ceiling summary: ~8.4M points ± sustained clock, gated on one missing file and
guarded by a self-test that already exists.*
