# Obsidio — session findings, 2026-08-23

> **Historical snapshot (2026-08-23):** the measurement record for the
> `perf/ryzen-ceiling` session, not a current guide. It was written while the
> work was scattered across three branches; §9 describes that state and is now
> out of date. What has changed since, and only this:
>
> - Both branches are merged into `main`. `phase-split-kernel.md` and
>   `wide-block2-negative-result.md` are committed under [`docs/`](..);
>   `avx_transition.cpp` and the whole `bench/ryzen/` harness are on `main`.
> - The `ab.sh` `RISK_PCT` fix of §4.2 has landed, so the "working copy only"
>   caveat there no longer applies.
> - The epoll fast-path work of §5 landed as `64689a7`, and lane-width batching
>   -- the fix for §2 step 2's x3 defect -- as `b37408b`. Both are unmeasured at
>   system level; re-baselining them is Phase 0 of
>   [`HOW-TO-TEST.md`](../../HOW-TO-TEST.md)'s successor plan.
> - §4.1's suggested fix is done: see
>   [`rnds2_ports.cpp`](../../starters/cpp/bench/x86/rnds2_ports.cpp).
>
> Every measurement below is left exactly as it was taken, including the
> hypotheses this session falsified. That is the point of this directory.

Machine: **AMD Ryzen 7 170**, Windows 11, Docker Desktop / WSL2, 16 threads.
Repo: `C:\Users\YASH\achal-projects\obsidio`. Branch under test:
`perf/ryzen-ceiling` (`8a6e653`), which sits on `main` at `dce9776`.

All five steps of that branch's `HOW-TO-TEST.md` were executed, plus two
follow-up experiments that were not in the plan.

---

## Bottom line

**Nothing in this session raised the score.** Every result below is a
measurement, a negative, or a bug — no optimisation was shipped. What the
session bought is direction: it identified the one remaining change with real
score in it, closed two dead ends cheaply, and caught two measurement bugs that
would each have written a wrong conclusion into the record.

The headline grading run came out at **work_score 5,767,977 with all four
thresholds green and 0.00% failures** — consistent with the machine's known
cool-state figure, so the branch introduced no regression.

| Question | Answer | Worth |
|---|---|---|
| Is `SHA256RNDS2` latency or port bound? | **Latency bound** (L=4, T=1) | the only real lead: +24% staged, ~+71% full |
| What does `SCHED_IDLE` cost? | **Nothing** — no arm beat the default | closed, negative |
| What does the cheap path cost? | **18.1 µs CPU/request** — genuine work | closed; caps other levers |

---

## 1. The one real lead — the phase-split kernel

Full design brief is committed in the repo at
[`phase-split-kernel.md`](../phase-split-kernel.md).
Summary:

`SHA256RNDS2` has 4-cycle latency and **1-cycle reciprocal throughput**. The
shipped two-lane kernel issues one instruction every two cycles into a unit
that accepts one per cycle, so it runs at roughly **half** the rate the
silicon sustains. The risk chain is ~91% of server CPU, so this is where the
money is.

Normalising both benchmarks to ns per `rnds2` (3.2M `rnds2` per chain) is what
makes them comparable:

```
shipped kernel, 2 lanes   0.4878 ns/rnds2
pure rnds2,     2 lanes   0.4245 ns/rnds2   <- kernel is only 15% off at 2 lanes
pure rnds2,     4 lanes   0.2473 ns/rnds2
pure rnds2,     6 lanes   0.2207 ns/rnds2   <- 2.2x below the shipped kernel
```

**The kernel is efficient for the width it runs at. It is running at the wrong
width.**

### The finding that changes the plan

The "6 XMM registers per lane, so 2 lanes is the ceiling" argument in
`chain_x86.cpp` is correct — **but it only applies to block 1.** Block 2 of
every steady-state hash is fixed padding, so its entire message schedule is
precomputed into `KW2V[]`. `compress2_const` already uses **zero
`sha256msg1`/`msg2` and 2 registers per lane**.

Block 2 is *already in phase-split form*. It is half of all the `rnds2` work,
needs no redesign whatsoever, and is pinned at two lanes only because the
function it sits inside is a two-lane function. That turns "rewrite the kernel"
into a staged plan whose first stage is nearly free:

| Stage | Change | Projected | **Measured** |
|---|---|---|---|
| **1** | Widen block 2 only (`compressN_const`, N=4–6) | ≈ +24% | **−23%. Built, verified, rejected.** |
| **2** | Phase-split block 1: schedule to an L1 buffer, 2 regs/lane in the round phase | ≈ +71% | not attempted |

**Stage 1 was built and measured after this report was first written, and it is
a 23% regression.** See §1a. Stage 2's figure was already the least trustworthy
number here — it uses the good end of a noisy plateau, and the bad end gives
roughly +45% — and Stage 1's failure is a direct reason to distrust it further.

### 1a. Stage 1 was built, and it does not work

Full write-up:
[`wide-block2-negative-result.md`](../wide-block2-negative-result.md), written
on `perf/wide-block2` and since merged. `src/` there is identical to `main` —
nothing shipped.

Block 2 scales in isolation exactly as argued: 2358 Mrnds2/s at two lanes
against **4361 at four** (peak; 4013 at six, 3057 at eight). Splitting the
shipped 62.44 ns two-lane group-round with that number projects a 1.249× risk
path, **+22.2%**.

Built and measured, `chain4` with a four-lane block 2 runs at **0.769×** — a
23% regression. Digests verified byte-identical throughout (`verify_lane4`, the
forced `RISK_BACKEND=x86-sha-ni` selftest, and a direct four-chain comparison).

The diagnostic is the useful part. Keeping the new structure — four chain
states live across both `compress2` calls — but doing block 2 as two 2-lane
passes measures **0.767×, identical**. So:

- the entire loss is the cost of **carrying four chain states through block 1**,
  whose own two lanes want 12–14 registers, spilling the waiting pair inside a
  64-round schedule loop;
- widening block 2 recovers **nothing net** — its inputs became spilled state
  that must be reloaded, so its lane width stopped being the constraint.

Every input to the +22.2% was correctly measured. The model joining them was
wrong: it treated the two blocks as separable when the chain state has to cross
the boundary between them.

This does not refute Stage 2, which separates the schedule phase from the round
phase and so never holds N states through register-hungry code. It does kill
the premise that there is a cheap down payment on it. **Before writing Stage 2,
measure what it costs to keep N chain states live across a phase boundary** —
the quantity that decided this, and the one nobody thought to measure.

### Why it might not pay

1. The 6-lane column ranged 0.2207–0.2895 across three reps.
2. The microbenchmark runs bare `rnds2` with no memory traffic. Stage 2 adds
   one 16-byte load per `rnds2`. Loads issue on different ports so this
   *should* be free, but it is **unmeasured** — the single assumption most
   worth testing before writing any kernel.
3. Phase-splitting lengthens the per-lane dependency chain; below ~4 lanes it
   is a regression.
4. `risk_pool` currently batches at most 4 and must widen in step.
5. Wider batches raise per-batch latency (very likely free — risk p95 has 7.4×
   margin and is queueing-dominated — but it is a real coupling).

### Positive evidence for the design

Four 128-bit vector ops per `rnds2` cost **0–11%**; four 256-bit ops cost
**98.5%** (see §4.1). The message schedule costs ~1.5 vector ops per `rnds2`,
less than half the probe's four, so there is spare vector issue capacity to
hide the schedule behind the round phase — provided everything stays 128-bit
and never reaches for AVX2. Budget for it costing something rather than
nothing.

---

## 2. Measured results

### Step 0 — sanity

`ctest` in the build image: **100% tests passed, 0 failed out of 2**.

### Step 1 — is `rnds2` latency or port bound? (3 reps)

| lanes | rep 1 | rep 2 | rep 3 | best ns/rnds2 |
|---|---|---|---|---|
| 1 | 1.00× | 1.00× | 1.00× | 0.867 |
| 2 | 2.00× | 1.61× | 2.11× | 0.4245 |
| 3 | 2.99× | 2.98× | 3.10× | 0.2897 |
| 4 | 3.05× | 3.56× | 3.14× | 0.2473 |
| 6 | 3.00× | 3.99× | 3.28× | 0.2207 |
| 8 | 3.64× | 3.96× | 3.70× | 0.2223 |
| **printed headline** | **8** | **6** | **8** | — |

Ran three reps, not the prescribed two, because the first two disagreed.

- **The printed `lanes to saturate` is argmax over a noisy plateau** (8, 6, 8
  across identical runs). Do not quote it.
- The reproducible facts are the 1-lane rate (0.867–0.897 ns, ±1.7%) and the
  3-lane rate (2.98–3.10×, the tightest point on the curve). Scaling clearly
  continues well past two lanes.
- At ~4.3 GHz: 0.87 ns ≈ **4 cycles latency**; the ~0.22 ns plateau ≈
  **1 cycle reciprocal throughput**.

### Step 2 — what the shipped kernel achieves

| | chains/s | ms/chain | ns/rnds2 |
|---|---|---|---|
| x1 | 241.21 | 4.146 | 1.296 |
| **x2 (shipped)** | **640.46** | **1.561** | **0.4878** |
| x3 | 390.84 | 2.559 | 0.800 |
| x4 | 658.28 | 1.519 | 0.4747 |

- `x4` ≈ `x2` because `chain4_impl` is two sequential `chain2_impl` calls —
  four jobs, still two lanes.
- **`x3` is 39% slower than `x2`.** `chain3_impl` is `chain2 + chain1` and the
  odd lane runs at the 1-lane rate. A three-job batch is worse than a two-job
  batch. This is a live defect, not just a missed optimisation.

### Step 3 — `RISK_SCHED` sweep (6 arms × 3 reps)

```
arm           n          mean   spread%   max fail%     cpu%
idle          3    883095.333     1.27%      0.000%   198.1%
nice19        3    885416.667     0.13%      0.000%   198.9%
nice10        3    876775.333     2.09%      0.000%   192.4%
nice5         3    876024.000     3.77%      0.000%   198.4%
batch         3    859427.333     5.70%      0.000%   197.8%
nice0         3    866751.333     3.41%      0.000%   196.1%

nice19 vs idle: +0.26%   INCONCLUSIVE
nice0  vs idle: -1.85%   INCONCLUSIVE
batch  vs idle: -2.68%   INCONCLUSIVE
```

**Hypothesis falsified.** The claim under test was that `SCHED_IDLE` costs
"possibly ~20% of the score". The largest effect in any direction is 2.7%, and
it is negative. Nothing beat the shipped default.

**Mechanism:** `cpu%` is ~196–199% in *every* arm including `idle`. The
hypothesis required the weight-3 workers to be starved, which would show as
idle-arm cpu% well below the 200% ceiling. Both granted CPUs are already
saturated as shipped. There is no stranded capacity for another scheduling
class to reclaim, so the 22.6% gap between the risk-only probe and the graded
mix is **not** scheduler weight. `price_p95` never moved off ~0.33 ms in any
arm, so no part of the 595× latency margin was being usefully spent either.

**Caveat, stated because the plan demands it:** this run does not meet its own
quality bar — worst per-arm spread 5.70% (target ~1%) and two `CLOCK DOWN`
warnings in rep 3 (nice5 82.4%, batch 83.4%). Those two throttled reps are
exactly what drags `batch` and `nice5` down, so the apparent negative deltas
are thermal, not scheduling. That failure mode only matters for resolving small
deltas, and a 20% effect would be unmissable at this noise level. A clean
`--cooldown 60` rerun is ~33 min if the record needs it; it will not change the
verdict.

### Step 4 — what the cheap path costs

| arm | score | cpu% | spread | CPU per cheap request |
|---|---|---|---|---|
| `IO_THREADS=2` | 4,198,406 | 126.9% | 2.73% | **18.1 µs** |
| `IO_THREADS=1` | 3,547,998 | 90.3% | 0.34% | 15.3 µs |

Against the plan's own calibration (~5 µs → mostly scheduling waste and
recoverable; ~50 µs → the IO path genuinely needs it), **18 µs sits near the
"genuine" end**, 3.6× the recoverable point. It independently corroborates the
~19 µs/request figure in [`score-levers.md`](score-levers.md) §5, derived a
completely different way.

Two consequences:

- **`IO_THREADS=1` is a 15.5% loss.** That lever is answered negative on x86.
- It downgrades the epoll fast-path work (see §5): at 18 µs with ~3 syscalls
  per request, eliding one saves ~1–2 µs of a path that is 9% of CPU — roughly
  **1% of score, not the +2–4% previously claimed**.

### Step 5 — full grading script, unmodified

```
work_score .................. 5,767,977   (21,363/s)
http_req_failed ............. ✓ rate<0.01    rate=0.00%   (0 of 2,307,098)
{tier:price} ................ ✓ p(95)<200    p(95)=328.53µs    609x margin
{tier:stats} ................ ✓ p(95)<500    p(95)=331.16µs   1510x margin
{tier:risk} ................. ✓ p(95)<1500   p(95)=203.36ms     7.4x margin
checks_succeeded ............ 100.00%
```

5.77M matches the previously recorded cool-state figure of 5.78M almost
exactly, so the box was cool and this is a good-case number, not a hot one. No
regression from the branch. Not repeated with a "winner" setting because step 3
produced none.

---

## 3. Negative results — doors closed

- **Scheduler tuning is dead.** Both CPUs already saturated; no stranded
  capacity. (Step 3)
- **`IO_THREADS=1` is dead on x86** — a 15.5% loss. (Step 4)
- **`x3` batching is actively harmful** — 39% slower than `x2`. (Step 2)
- **The "x3/x4 gain exactly zero, forever" branch is falsified.** This was the
  scenario in which the entire wider-kernel line of work would have been
  deleted. It is not the case. (Step 1)

---

## 4. Harness bugs found

Both would have written a wrong conclusion into the permanent record.

### 4.1 `starters/cpp/bench/x86/rnds2_ports.cpp` — AVX2 "port contention" is a transition artifact

The benchmark reports AVX2 co-issue costing SHA-NI ~98.5% and concludes *"they
compete for issue slots; the hybrid stays dead."*

That is an **AVX/SSE transition penalty**, not port contention.
`sha256rnds2` has **no VEX encoding** — it is legacy-SSE only, so a legacy-SSE
write leaves the upper 128 bits of the YMM register dirty. Interleaving it with
VEX-256 AVX2 in the same loop pays that penalty every iteration.

Measured directly with a purpose-written probe:

Measured across five runs:

```
                          run1     run2     run3     run4     run5
+ 4x SSE 128-bit adds    -0.0%    -9.9%    -2.7%    +0.4%   -10.8%
+ 4x AVX2 256-bit adds  -98.5%   -98.5%   -98.6%   -98.6%   -98.5%
+ 4x AVX2 + vzeroupper  -99.3%   -99.3%       —        —        —
```

128-bit co-issue costs **0–11%** — noisy and bimodal, so not free, but two
orders of magnitude cheaper than the 256-bit arm, which is stable to ±0.1%.
The issue ports are therefore demonstrably not saturated, and `vzeroupper`
making it *worse* confirms the transition itself is the cost. **The SHA-NI +
AVX2 hybrid is not closed by this evidence** — and, more usefully, this is
evidence that the phase-split kernel's message-schedule work can largely hide
behind the round phase.

Probe source: written in job-scratch as `avx_probe.cpp`, since committed as
[`starters/cpp/bench/x86/avx_transition.cpp`](../../starters/cpp/bench/x86/avx_transition.cpp)
and built by the image as `obsidio-avx-transition`.

Suggested fix: have the co-issue arm use 128-bit lanes, or report the
transition penalty as its own separate finding.

### 4.2 `starters/cpp/bench/ryzen/ab.sh` — `RISK_PCT` never reaches k6

`run_probe()` forwards only `TARGET`, `DURATION` and optionally `VUS` into the
k6 container. `mixed_probe.js` reads `__ENV.RISK_PCT` and defaults to 10.

So the documented step 4 command —

```bash
RISK_PCT=0 ./ab.sh --probe mixed --reps 3 "cheaponly:IO_THREADS=2" ...
```

— silently measured the **normal 10%-risk mix** while labelling it cheap-only.
Every CPU-per-cheap-request figure derived from it would have been wrong.

One-line fix:

```bash
[ -n "${RISK_PCT:-}" ] && extra+=(-e "RISK_PCT=$RISK_PCT")
```

Confirmed by effect: with the fix, score jumps from ~880k to ~4.2M, which is
what removing `/risk` should do. **The step 4 numbers in §2 are from the fixed
harness.** The fix was applied only to a working copy; the branch file is
unmodified.

---

## 5. Related work in flight (not on this branch)

An implementation of the epoll fast-path lever (`score-levers.md` §5) is
**stashed on `main`** as `stash@{0}` — `lever5-fastpath-wip`:

- `Connection::armed` tracks the registered epoll mask; `arm()`/`disarm()` skip
  `epoll_ctl` when the interest set is already correct.
- Events carry `Connection*` in `ev.data.ptr` instead of `data.fd`.
- Added deferred reclamation (`closed` flag + end-of-batch close list). The
  original §5.2 proposal as written was **unsafe** — raw pointers in a
  snapshotted `epoll_wait` batch can dangle when a completion drain closes a
  connection whose own event sits later in the same batch. Deferring the free
  also fixes a pre-existing fd-reuse aliasing bug where a stale `EPOLLHUP`
  could kill a freshly accepted connection.

Verified: clean at `-Wall -Wextra`; full Docker build gate passes (4 selftest
passes + 56 HTTP checks); ASan/UBSan build passes both suites.

Measured baseline: **1004 `epoll_ctl` calls for 1000 keep-alive requests** —
one wasted syscall per request, confirming the premise. The after-count run was
interrupted and is **still unproven**. Expected ~2.

Per §2 step 4, its realistic value is now **~1% of score**, not the +2–4%
originally claimed.

---

## 6. Corrections to existing repo documents

| Location | Status |
|---|---|
| [`score-levers.md`](score-levers.md) §4 — "if recip-tput is 2 cyc, x3/x4 dead forever" | **Falsified.** T=1 cycle; the wider-kernel line is live |
| [`score-levers.md`](score-levers.md) §3 — "close the ~20% gap to the two-lane floor" | **Wrong target.** The kernel is within 15% of the two-lane floor already; the gap worth chasing is lane count |
| [`score-levers.md`](score-levers.md) §5 — epoll fast path worth +2–4% | **Downgraded to ~1%** by the measured 18 µs cheap-path cost |
| [`score-levers.md`](score-levers.md) §9 — `IO_THREADS=1` retest, "+3–4%?" | **Negative on x86**: a 15.5% loss |
| [`score-levers.md`](score-levers.md) §6 — 3-job batches run one lane at half rate | **Confirmed empirically**: x3 is 39% slower than x2 |
| [`HOW-TO-TEST.md`](../../HOW-TO-TEST.md) step 1 — "the hybrid stays dead" | **Not supported** by that measurement (§4.1) |
| [`HOW-TO-TEST.md`](../../HOW-TO-TEST.md) step 3 — `SCHED_IDLE` costs ~20% | **Falsified** (§2 step 3) |

---

## 7. Reproducing this on this machine

Two environment gaps, both of which the test plan warns about and both of which
are real here:

- **No WSL2 distro exists** — only `docker-desktop`. `wsl -e bash` fails, so
  the plan's required shell is unavailable.
- **No host `python3`** — only the Windows Store alias stub. `ab.sh` needs it
  for both the clock probe and the summary table.

Workaround used, which works cleanly:

1. Run the harness **inside a container** (`docker:cli` + `bash` + `python3`)
   with `/var/run/docker.sock` passed through, so it drives sibling containers.
2. Bind-mount the repo at the **same absolute path the host daemon resolves**
   (`-v "C:/path:/c/path"`), so `ab.sh`'s nested `-v "$HERE:/scripts:ro"` still
   points at real host files rather than at the harness container's filesystem.
   The legacy `/c/Users/...` form does resolve under Docker Desktop — verified.
3. Make an **LF copy** of `bench/ryzen/*`. The Windows checkout is CRLF and
   bash rejects `set -uo pipefail\r`.

From Git Bash, prefix any `docker run` with a `-v` flag with `MSYS_NO_PATHCONV=1`
or the path gets mangled.

---

## 8. Suggested order of work

1. **Extend `rnds2_ports.cpp`** with a memory-sourced `W+K` variant and a
   `msg1`/`msg2` co-issue arm. ~1 hour, and it de-risks the two assumptions
   Stage 2 rests on.
2. **Backend lane-width field**, with `risk_pool` batching to `backend->lanes`
   (2 for x86, 4 for ARM). Fixes the x3 defect. Do **not** hardcode 2 —
   `chain_arm.cpp` is a genuine four-lane interleave and would be halved.
   Small, portable, provably not worse, and it is the seam Stage 1 needs anyway.
3. **Stage 1**: `compressN_const`. Measure against `x2` on the same box.
4. **Stage 2** only if 1–3 hold up.
5. Separately: unstash and land the epoll fast-path work, and finish its
   syscall-count verification.

---

## 9. Repository state at end of session

*Superseded — see the banner at the top of this file. Kept as the record of
what the consolidation that followed had to reconcile.*

- On branch `perf/ryzen-ceiling`. The branch's own files were **never edited**.
- `docs/phase-split-kernel.md` written, **untracked** — not yet committed.
- `stash@{0}` (`lever5-fastpath-wip`) holds the epoll work, taken on `main`.
- `main` locally is 2 commits ahead of `origin/main`: the score-levers doc
  rebased onto the real main, plus a commit filing the three analysis notes
  under `docs/history/` with their cross-references repaired. **Not pushed.**
- No containers running; temporary worktree removed.

*Every absolute number here is one laptop under Docker Desktop / WSL2. The
reasoning is portable; the numbers are not.*
