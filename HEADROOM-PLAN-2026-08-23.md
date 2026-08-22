# Claiming the headroom: plan of record, 2026-08-23

Companion to [`docs/history/ryzen-ceiling-findings.md`](docs/history/ryzen-ceiling-findings.md).
Everything here is derived from that session's measurements,
[`docs/phase-split-kernel.md`](docs/phase-split-kernel.md) (with its Stage 1
retraction), and
[`docs/wide-block2-negative-result.md`](docs/wide-block2-negative-result.md).
All three were on separate branches when this was written; Phase 0 merged them.

**The claim being planned against:** `SHA256RNDS2` is latency bound (L=4,
T=1), the shipped two-lane kernel runs at ~half the instruction's sustainable
rate, and the risk chain is ~91% of server CPU. The honest value band for
closing that gap is **+15% to +45%** of `work_score` (the +71% figure uses the
good end of a noisy plateau and a projection style that has already missed by
45 points once — treat it as the ceiling, not the estimate).

**The lesson this plan is built around:** Stage 1 died because a projection
joined two separately-measured halves across a boundary the chain state has to
cross. So this plan never spends kernel-writing days on arithmetic. Every
phase ends in a numeric gate measured *in the structure that will ship*, and
failing a gate costs at most a day.

---

## Ledger — what is already banked, dead, or live

| Item | Status |
|---|---|
| Backend lane-width batching (fixes the x3 −39% defect) | **Landed on `main`** (`b37408b`), system effect unmeasured |
| epoll re-arm elision + `data.ptr` connections | **Landed on `main`** (`64689a7`), ~1% expected, unmeasured |
| Harness fixes: `RISK_PCT` forwarding, AVX/SSE transition isolation | **Committed on `perf/ryzen-ceiling`** (`69f4f91`), unmerged |
| Stage 1 (wide block 2) | **Dead**: −23% measured; retraction on both branches |
| `SCHED_IDLE`/scheduler tuning, `IO_THREADS=1`, x3 batching | **Dead**, closed by measurement |
| Stage 2, sequential phase-split | **Dead**: built as a verified skeleton, measures +6.2% against a +15% floor. See Phase 1 result |
| Stage 2, **pipelined** phase-split | **SHIPPED**: integrated as `chain8`, graded at 6,664,861 (+14.5%). See Phase 2 result |
| Pipelined kernel, **two streams per block** (probe mode A) | **SHIPPED — final design change**: all four schedules ride the rounds, bench 1.581× vs x2, graded at **`work_score` 7,360,030** — **+26.5%** over the 5,818,877 baseline, risk p95 160.06 ms, 0.00% failures. Confirmed by an independent final run at **7,365,605** (+0.08%, clean 4m30.0s clock — which also retires the stretched-clock caveat on the first run), risk p95 159.79 ms. Design work is closed; what remains is cleanup, docs, and the pitch |
| Baseline | **5,818,877 cool-state**, 2026-08-23, after both landed perf commits (was 5,767,977 before them — see Phase 0 result) |

---

## Phase 0 — consolidate and re-baseline (~half a day)

Nothing new gets measured well while the tree is scattered across three
branches and two untracked files.

1. **Merge `perf/ryzen-ceiling` into `main`** (bench instrumentation,
   `avx_transition.cpp`, the two harness fixes, `phase-split-kernel.md` with
   its retraction banner). Merge `perf/wide-block2`'s doc and
   `bench_wide_block2.cpp` too — the negative result and its control bench are
   exactly what Phase 1 builds on. `main` is ahead of origin and stays local
   until a reviewer has diffed the lot.
2. **Commit `OBSIDIO-FINDINGS-2026-08-23.md`** (move under `docs/history/` to
   match the filing convention — it landed as
   [`ryzen-ceiling-findings.md`](docs/history/ryzen-ceiling-findings.md), since
   that directory names by subject and not by date).
3. **Close the flagged follow-up in `69f4f91`**: `rnds2_ports.cpp`'s verdict
   text still asserts port saturation that its own companion bench refutes.
4. **Codify the Windows/Docker harness workaround** (findings §7) as a script
   or README section under `bench/ryzen/`: the `docker:cli` harness container
   with the socket passed through, the same-absolute-path bind mount, the LF
   copy, `MSYS_NO_PATHCONV=1`. It was re-derived once already; make reruns a
   one-liner.
5. **Re-baseline.** One full unmodified grading run, cool box (idle ≥15 min,
   no `CLOCK DOWN` warnings). This captures the landed lane-batching and epoll
   work — expected +1–3% over 5.77M — and becomes the reference every later
   A/B is judged against. Do not skip this: attributing those percents to the
   kernel later would corrupt the Stage 2 verdict.

**Exit state:** one branch, one baseline number, a rerunnable harness.

### Phase 0 result — executed 2026-08-23

Done: both branches merged (`src/` byte-identical to `b37408b` throughout, tree
`0fdb99d`), the findings filed under `docs/history/`, `rnds2_ports.cpp`'s
falsified verdict retracted, and the Windows harness workaround codified as
`bench/ryzen/run-harness-windows.sh`.

**Baseline of record: `work_score` 5,818,877.** Full unmodified grading script,
cool box (15 min idle), on AC (`Win32_Battery.BatteryStatus = 2`,
`PowerOnline = True`).

```
work_score .................. 5,818,877   (21,552/s)
http_req_failed ............. ✓ rate<0.01    rate=0.00%   (0 of 2,328,528)
{tier:price} ................ ✓ p(95)<200    p(95)=321.70µs    622x margin
{tier:stats} ................ ✓ p(95)<500    p(95)=324.33µs   1542x margin
{tier:risk} ................. ✓ p(95)<1500   p(95)=203.65ms     7.4x margin
checks_succeeded ............ 100.00%
effective clock ............. 6.545 -> 6.497 (99.3%, no sag)
```

**+0.88% over the 5,767,977 reference — do not bank that as the +1–3%.** The
epoll and lane-batching commits are real work and may well be worth it, but
this instrument cannot resolve a percent: `HOW-TO-TEST.md` says in as many
words not to trust the grading script under ~10%. Phase 0's contribution is the
*reference*, not a demonstrated gain. Treat +0.88% as "consistent with anything
from zero to a few percent" and use `ab.sh`'s alternated A/B if the landed
commits ever need a number of their own.

The `{tier:risk}` p95 barely moved (203.65 ms against 203.36 ms), which is what
a queueing-dominated latency should do when throughput moves by under a
percent.

---

## Phase 1 — three probes, three kill gates (~1 day)

The three unmeasured assumptions Stage 2 rests on, cheapest first. Each probe
has a numeric gate; failing any gate kills Stage 2 for the cost of hours.

Scoring arithmetic used throughout (91/9 CPU split): a risk-path speedup `r`
gives `score × 100 / (91/r + 9)`. So r=1.17 → +15%, r=1.25 → +21%,
r=1.5 → +43%, r=1.84 → +71%. Shipped kernel: 0.4878 ns/rnds2.

### Probe B — memory-sourced `W+K` (~1 hour)

Extend `rnds2_ports.cpp`: each `rnds2` takes its round constant from a
16-byte L1 load instead of a register. Sweep lanes 2/4/6, 3 reps, cooldown.

> **Gate B:** worst-rep 4-lane rate ≤ **0.30 ns/rnds2**. The bare plateau is
> 0.247–0.290; if one load per `rnds2` pushes it past 0.30, the round phase
> alone can no longer clear the +15% floor once schedule overhead is added.

### Probe C — schedule co-issue (~1 hour)

Same bench, an arm that co-issues the *real* schedule instruction mix
(`sha256msg1`/`msg2`, `alignr`, `add` — ~1.5 vector ops per `rnds2`, all
128-bit, never AVX2) alongside the `rnds2` lanes.

> **Gate C:** co-issue cost ≤ **15%** at 4 lanes, worst rep. The
> `avx_transition.cpp` result (four 128-bit ops cost 0–11%) says this should
> pass; if the real mix costs materially more, the "schedule hides behind the
> rounds" premise is wrong and the block-1 estimate inflates past usefulness.

### Probe A — state carry across the phase boundary (~half a day, the decisive one)

The quantity that killed Stage 1 and was never measured. Build a skeletal
Stage-2 inner loop — not a full kernel: per group-round, a schedule phase
writing N lanes' 64-entry `W+K` buffers (using the existing 6-register
schedule sequence, states *not* live), then a round phase interleaving N lanes
at 2 registers each reading from those buffers, block 2 from `KW2V` as today.
Real register pressure, real structure, constant-message correctness checks.
`bench_wide_block2.cpp` is the pattern to extend. (Its build quirk is gone:
`bench/` was outside the Docker context on `perf/wide-block2` and the source
had to be mounted by hand, but the Dockerfile that came in with the
`perf/ryzen-ceiling` merge copies `bench/`, so `docker build --target build`
produces `obsidio-bench-wide-block2` directly.)

Measure ns/rnds2 at N=2, 3, 4, 6. N=2 is the control — it must land near the
shipped 0.4878, or the skeleton itself is wrong.

> **Gate A:** worst-rep N=4 (or N=6) ≤ **0.39 ns/rnds2** — i.e. the structure
> that will actually ship beats the shipped kernel by ≥25%, leaving margin for
> integration losses over the +15% score floor.

### Phase 1 exit

Re-project Stage 2 using *only* Probe A's joint measurement — not by combining
B and C arithmetically; B and C exist to explain a Probe A failure, not to
substitute for it. Proceed to Phase 2 only if the worst-rep projection clears
**+15% score**. If any gate fails, Stage 2 is dead: write the negative result
into `docs/` (the Stage 1 write-up is the template) and go to Phase 4.

Probe hygiene for all three: 3+ reps, `--cooldown 60`, in-run ratios rather
than absolutes, quote the worst rep, never quote an argmax over the plateau.

### Phase 1 result — executed 2026-08-23. Stage 2 is dead.

Full write-up: [`docs/phase-split-negative-result.md`](docs/phase-split-negative-result.md).
Benches: `obsidio-stage2-probes` (B and C), `obsidio-bench-phase-split` (A).
Cool box, on AC, worst rep of 7, digests verified against `risk_hash()` before
any timing.

| Gate | Threshold | Measured | Verdict |
|---|---|---|---|
| **B** — memory-sourced `W+K` | ≤ 0.30 ns/rnds2, 4 lanes | **0.2490** (+10.3% over a register operand; +15.2% in the shipped load-per-pair shape) | **PASS** |
| **C** — schedule co-issue | ≤ 15% at 4 lanes | **+225%** | **FAIL** — but on registers, not ports |
| **A** — the joint structure | ≤ 0.39 ns/rnds2 | **0.5106** (1.069× risk path, **+6.2% score**) | **FAIL** |

**Gate C's failure is not what it looks like.** `objdump` shows the co-issue arm
spilling 42 stores and 37 reloads per iteration — four lanes want 2 state
registers each plus 5 schedule registers each, 28 in a file of 16. Sweeping
schedule streams against fixed round lanes separates the two effects: one
stream is nearly free and raises total SHA-unit throughput *above* the
rounds-only rate, so the issue ports do have slack and Gate C's stated premise
is confirmed. The cliff is between 13 registers and 18.

**Gate A is the one that decides, and it fails cleanly.** The round phase works:
0.3058 ns/rnds2 at four lanes against the shipped 0.5456 is **1.78×**. But
phase-splitting converts the message schedule from work that hides inside
`sha256rnds2` latency into work that must be paid for — 46% of the group-round
at four lanes — and 1.78× on the rest does not buy it back.

Two corrections to this plan, recorded because they cost time:

- **The N=2 control was mis-specified.** "It must land near the shipped 0.4878
  or the skeleton is wrong" fires on the design's own documented behaviour —
  `phase-split-kernel.md` already said phase-splitting is a regression below ~4
  lanes. It measured 0.8178 with correct digests. A control has to be a
  configuration where old and new are *supposed* to agree.
- **Gate C's arm does not match Stage 2's structure.** Stage 2 phase-splits
  precisely so N schedules and N chain states are never live together; the gate
  measures co-issuing them. It should have been written as a port-capacity
  probe with fixed register pressure.

One branch is left open and is *not* authorised here: software-pipelining the
two phases, so the schedule for round *r+1* hides inside the round phase for
*r*. **43.5%** of the schedule has to disappear for 1.17×; one co-issued stream
supplies 1070.7 M msg-ops/s against a requirement of 1178 M/s, so on paper it
covers ~91% and the design projects **≈ +51%**.

That paper is not to be trusted — it lands above the plan's own +45% ceiling
off a 13-register measurement extrapolated onto a structure nobody has built,
and the honest band is **−15% to +61%**, hinging entirely on whether the XMM
file holds when a 5-register schedule stream is added to Stage 2's real round
phase. Probe C's streams=2 row is what the downside looks like. So: exactly one
more probe, hard gate at 1.17×, never a kernel — but it is now the
**highest-expected-value work left**, ahead of Phase 4's mechanical +3–6%. §6
of the write-up has the full arithmetic and the correction history.

**Phase 2 is not entered.**

### Phase 1a — the pipelined probe was authorised and run. It passes.

**Gate P (worst-rep in-run ratio vs shipped x2 ≥ 1.17×): PASS at 1.371× worst
across four clean runs, 1.413× cool — ≈ +33% to +36% `work_score`.**

The design had to change on a constraint §6 missed: round *r+1*'s schedule
cannot hide inside round *r*'s round phase **for the same lane**, because the
schedule's input is that round phase's own output. The pipeline therefore runs
across lane *groups* — `rounds(A) ‖ schedule(B)`, then `rounds(B) ‖
schedule(A′)` — which keeps at most one schedule stream live and is why the
register file held where Probe C's naive co-issue blew through it.

Cool box, worst rep of 7, against 0.5200 ns/rnds2 shipped:
N=4 → 1.176×, N=6 → 1.194×, **N=8 → 1.413×**. Held fixed at the same
round-phase width, co-issuing the schedule is worth **27%**.

It landed between the gate and the model, not at §6's +51% — the round phase is
only N/2 lanes wide, only about half the schedule is actually co-issued at
HALF ≥ 3, and it takes eight chains to get there.

**Phase 2 remains unauthorised.** What it would now involve is larger than this
plan scoped: an eight-wide risk batch, `Backend::lanes` and `risk_pool` widened
well past the `RiskJob jobs[4]` hardcap, and a per-batch latency that roughly
triples (≈ 9.4 ms against 3.3 ms) into a p95 with 7.4× of margin. Full detail
and the open questions are in §6a of the write-up.

### Phase 2 — authorised, built, shipped. Executed 2026-08-23.

The pipelined kernel went into `src/` as `chain8`, `Backend::lanes` went to 8
on x86 (ARM untouched at 4), and the pool batches eight-when-eight-are-queued
with sub-eight groups composing exactly as before. Full correctness ladder
green: Release ctest, forced `x86-sha-ni` and `reference` selftests, the
sanitised suite, and the Docker build gate.

One debugging detour that produced a portable fix: the sanitised server was
dying at startup in a DEADLYSIGNAL loop. Not the kernel, not a race — GCC 12's
ASan runtime vs `vm.mmap_rnd_bits=32`, a coin flip per process start that
**pre-exists this branch** (1/10 crashes on `main`, 5/10 on the bigger WIP
binary, 0/10 with ASLR off). Fixed with `-no-pie` on sanitised builds only;
20/20 clean starts after.

Measured, integrated, through the backend it registered (worst list in
`bench_chain_x86`): **x8 = 827.4 chains/s vs x2 = 639.9 → 1.293×** — the probe
promised 1.371× and integration paid ~6% in hex round-trips and call
boundaries. Digest-verified against chain2 before timing.

**Grading run (the decision gate): `work_score` 6,664,861 — +14.5% over the
5,818,877 baseline.** One run, cool box, AC verified. All four thresholds
green, 0.00% failures (0 of 2,667,659). And the plan's last "should" became a
number in the right direction: `{tier:risk}` p95 **fell** from 203.65 ms to
177.24 ms — the tripled per-batch latency is invisible under a queue that
drains 29% faster. `{tier:price}` p95 412.72 µs, still 484× of margin.

+14.5% sits at the +15% floor rather than at the bench's +26% projection: the
implied system-level risk speedup is 1.16× against 1.293× at the bench, the
gap being ramp phases running below eight-wide and everything else that is not
a deep steady queue. Follow-ups with known headroom, deliberately not taken in
this pass: co-issuing the second half of the schedule (only ~half hides at
HALF ≥ 3), and pool/wake tuning for the ramp. They are the next probes, not
assumptions.

---

## Phase 2 — the Stage 2 kernel (2–4 days, only after Phase 1 passes)

**Ask the organisers about the grading architecture before starting this
phase** (`score-levers.md` order-of-operations item 1, still open). Probes are
cheap enough to run regardless; kernel-days are not. Stage 2 is x86-only —
`chain_arm.cpp` is already a genuine four-lane interleave. If grading is ARM,
stop here and re-aim at the ARM ceiling instead.

### Design shape

Per group-round, for N lanes (N=4 first; 6 only if 4 ships and the plateau
says more is there):

1. **Schedule phase.** For each lane, expand its 64-byte message into an
   L1-resident `W[0..63]+K` buffer. The schedule depends only on the message —
   previous round's hex output — not on the chain state, so *no chain states
   are live here*. Reuses the existing schedule sequence from
   `compress_generic`; whether to do lanes singly or in interleaved pairs is a
   measured choice inside Probe A's skeleton.
2. **Round phase.** All N lanes interleaved at 2 registers per lane: block 1
   reads `W+K` from the buffers (the shape `compress_const_block2` already
   has, with a load instead of a table), block 2 from `KW2V`. N=4 costs 8
   registers plus temporaries; N=6 costs 12.

The structural difference from dead Stage 1, stated so nobody forgets it: N
chain states are only ever co-resident in code that costs 2 registers per
lane. The register-hungry schedule code never holds any chain state.

### Order of work

1. **Kernel in the bench first**, never in `src/` first. Alternated A/B
   against the shipped x2 in one process, digest-verified against the golden
   before any timing. **Ship gate: ≥ +20% at bench level, worst rep.**
2. **Integrate** as `chainN_impl` in `chain_x86.cpp`; bump `Backend::lanes`.
   The pool already batches to `backend->lanes` — the one hardcap to widen is
   `RiskJob jobs[4]` in `risk_pool.cpp`'s worker loop. Keep the composition
   rules for odd remainders (drain-mode groups) explicit and measured.
3. **Correctness ladder, in full, every time the kernel changes:** FIPS
   vectors; `verify_lane`-style direct comparison; forced
   `RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest` (must print "all checks
   passed", not SKIP); ASan/UBSan build over both suites; the Docker build
   gate. A wrong digest scores zero and looks exactly like a right one.
4. **Latency coupling check:** wider batches raise per-batch risk latency.
   Margin is 7.4× on a queueing-dominated p95, so this should be free, but
   confirm `{tier:risk}` p95 stays green in Phase 3 rather than assuming it.

---

## Phase 3 — system validation (~half a day)

1. **Alternating mixed-probe A/B** (`ab.sh`, the fixed harness with
   `RISK_PCT` forwarding), new kernel vs `main`, 3 reps, `--cooldown 60`,
   per-arm spread ≤ ~2%, no `CLOCK DOWN` warnings accepted in a quoted rep.
2. **Full unmodified grading script**, cool box, against the Phase 0
   baseline. Accept only with all four thresholds green and 0.00% failures.
3. Commit the numbers and the conditions they were taken under. One laptop
   under Docker Desktop/WSL2: the ratios are the deliverable, not the
   absolutes.

---

## Phase 4 — the floor plan, if Stage 2 dies

Banked regardless: Phase 0's +1–3%. Then the honest remainder, ranked:

| Lever | Expected | Notes |
|---|---|---|
| LTO / PGO / clang-vs-gcc sweep | +1–3% | untouched Release flags; mechanical |
| Fast-path profiling of the 18 µs | +1–2% | 18 µs is mostly genuine work, but double formatting and response-path allocations were never profiled |
| Spin-then-sleep workers | +0–1% | ramp-edge; `SCHED_IDLE` makes spinning safe |
| `vzeroupper` after round zero | +0–0.5% | five minutes; insurance against OpenSSL's AVX2 dirtying uppers |
| Pre-warm before bind; SMT affinity audit | small / insurance | check WSL2 topology passthrough |

**Asymmetric x3 is presumed dead** by the wide-block2 diagnostic — it carries
a third chain state across block 1's register-hungry code, the exact mechanism
that cost −23% with four states. Revisit only if Probe A happens to show N=3
carry is nearly free.

**The prize lever is claimable in every branch of this plan.** The track
judges a resilience write-up, and this repo now owns material almost no other
team will have: a −23% retraction with a controlled diagnostic, two harness
bugs caught before they wrote wrong conclusions into the record, and a
measurement discipline (alternated A/B, thermal gating, worst-rep quoting)
with receipts. Folding the Stage 1 story into `docs/resilience-writeup.md` is
hours of work with no measurement risk, and it pays out whether or not Stage 2
ships.

---

## Guardrails (apply to every phase)

- **Measure the joint, never the halves.** No projection built by combining
  separately-measured components crosses a phase gate. This is the rule Stage
  1 wrote in −23% ink.
- **Thermal discipline:** cool box for baselines, `--cooldown 60` for sweeps,
  alternated A/B for comparisons, worst rep quoted, `CLOCK DOWN` reps
  discarded.
- **Digest verification precedes every timing.** No exception, including in
  probes — a wrong digest scores zero and looks exactly like a right one.
- **Everything stays 128-bit.** The AVX/SSE transition penalty is ~98.5%;
  `sha256rnds2` has no VEX encoding. No AVX2 anywhere near the kernel.
- **File negative results.** A dead lever with a written cause is the cheapest
  thing this project produces and has already prevented two wrong records.

## Expected value, stated honestly

| Path | Cost | Value |
|---|---|---|
| Phase 0 alone | ~half a day | +1–3% banked, plus a clean tree and rerunnable harness |
| Phases 0–1, gates fail | ~1.5 days | the +1–3%, plus Stage 2 closed for good with a written cause |
| Phases 0–3, gates pass | ~4–6 days | **+15% floor, +45% realistic top end** on `work_score` |
| Phase 4 fallback | ~1–2 days | +3–6% combined, plus the prize write-up |

The gates exist because the last projection with this much upside missed by 45
points. Trust the structure, not the arithmetic.
