# The phase-split kernel: built as a skeleton, measured, and it does not pay

**Result: +6.2% projected `work_score` against a +15% floor. Gate A failed.
Stage 2 is closed. `src/` is untouched — nothing was shipped, and nothing
needed to be.**

This closes "Stage 2" of `phase-split-kernel.md`, the last live item in that
document after Stage 1 was retracted at −23%. Measured 2026-08-23 on a
Ryzen 7 170 / Docker Desktop / WSL2, on AC, cool box.

The plan that led here (`../HEADROOM-PLAN-2026-08-23.md`) was built around one
rule: never let a projection assembled from separately-measured halves cross a
gate, because that is the error that cost Stage 1 forty-five points. That rule
is why this took hours instead of the 2–4 kernel-days Phase 2 was budgeted.

---

## 1. What was measured

Not a projection. The whole structure, running end to end, verified, and timed
against the shipped kernel **in the same process on the same clock**:

- **Schedule phase.** Each lane's 64-byte message expanded into an L1-resident
  `W+K` buffer using `compress_generic`'s own schedule sequence, with every
  `RNDS2_PAIR` replaced by a store. Six registers live. No chain state.
- **Round phase.** N lanes interleaved at 2 state registers each. Block 1 reads
  the buffers instead of four live schedule registers; block 2 reads `KW2V`
  exactly as today. Real feed-forward, real hex round trip.

`bench/x86/bench_phase_split.cpp`, target `obsidio-bench-phase-split`.

**Correctness first.** Every configuration — N = 2, 3, 4, 6, both schedule
interleavings — was run for 512 rounds and compared against
`obsidio::risk_hash()` through the public API before anything was timed. All
match. A wrong digest scores zero and looks exactly like a right one.

## 2. The numbers

Worst rep of 7, cool box. The shipped x2 measured in the same process at
**0.5456 ns/rnds2** worst (0.4911 best), against 0.4878 recorded last session —
close enough to trust the in-run ratio, which is the deliverable.

| schedule | N | ns best | ns worst | vs shipped |
|---|---|---|---|---|
| one lane at a time | 2 | 0.6918 | 0.8178 | 0.667× |
| one lane at a time | 3 | 0.5529 | 0.6934 | 0.787× |
| one lane at a time | **4** | 0.4742 | **0.5106** | **1.069×** |
| one lane at a time | 6 | 0.5142 | 0.5695 | 0.958× |
| two lanes interleaved | 2 | 0.6541 | 0.6991 | 0.780× |
| two lanes interleaved | 4 | 0.4741 | 0.5368 | 1.016× |
| two lanes interleaved | 6 | 0.5240 | 0.5924 | 0.921× |

> **Gate A:** worst-rep N=4 or N=6 ≤ 0.39 ns/rnds2.
> **Measured 0.5106. FAIL**, by 31%.

At the 91/9 CPU split, 1.069× on the risk path is **+6.2% `work_score`**. The
floor was +15%. Interleaving two lanes' schedules helps at N=2 and hurts at 4
and 6, so that choice is settled too: it is not the lever.

The ratio is stable. Three independent runs — one warm, one cool, one with an
instrument bug since fixed — gave 1.073×, 1.069× and 1.144×.

## 3. Why — the decomposition

Re-running the round phase against a stale buffer isolates what the schedule
phase costs. This explains a measurement already taken; it is not an input to
any projection.

| N | full | round phase only | schedule | schedule share |
|---|---|---|---|---|
| 2 | 0.7172 | 0.4901 | 0.2271 | 31.7% |
| **4** | 0.5678 | **0.3058** | 0.2619 | **46.1%** |
| 6 | 0.5691 | 0.4479 | 0.1212 | 21.3% |

**The round phase works exactly as designed.** 0.3058 ns/rnds2 at four lanes
against the shipped kernel's 0.5456 is **1.78×** — the headroom the whole plan
was aimed at is real, and taking round constants from L1 does not cost it.
Probe B confirms that independently: one 16-byte load per `rnds2` costs +10.3%
over a register operand, one load per pair +15.2%, both far inside Gate B's
0.30 ns.

**And then the schedule has to be paid for, in full, in cash.** It is 46% of
the group-round at four lanes. In the shipped kernel that work is *free* — it
hides inside `sha256rnds2`'s four-cycle latency, in the gaps the two-lane
interleave cannot fill anyway. Phase-splitting is what makes it visible: the
schedule phase and the round phase run one after the other, and neither hides
the other.

That is the whole result. **Stage 2 does not fail because the round phase is
slow. It fails because it converts free work into paid work, and the round
phase's 1.78× is not big enough to buy it back.**

The N=6 row is the register wall showing up on schedule: 12 state registers
plus per-lane round temporaries exceeds the XMM file, the round phase degrades
from 0.3058 to 0.4479, and six lanes end up slower than four.

## 4. The N=2 control, and why the plan's reading of it was wrong

The plan said: *"N=2 is the control — it must land near the shipped 0.4878, or
the skeleton itself is wrong."* N=2 measures 0.8178, **150% of shipped**.

The skeleton is not wrong — the digests prove it computes SHA-256 correctly,
and the control criterion was. `phase-split-kernel.md`'s own risk list says
*"phase-splitting lengthens the per-lane dependency chain; below ~4 lanes it is
a regression."* At two lanes there is not enough interleaved round work to
absorb a serialised schedule phase, so the structure loses badly — which is
precisely what the design predicted and what the control then flagged as
instrument error.

A control that fires on the design's own documented behaviour is not a control.
Worth remembering the next time one is written: a control must be a
configuration where the new structure and the old one are *supposed* to agree.

## 5. What Probes B and C contribute

They exist to explain a Probe A failure, not to substitute for it, and they do.

- **Probe B: PASS.** Memory-sourced `W+K` is cheap — +10.3% at one load per
  `rnds2`, +15.2% in the shipped load-per-pair shape, against a 0.30 ns gate
  met at 0.2490. The buffer was never the problem.
- **Probe C: FAIL as written, +225% at four lanes** — and the mechanism is the
  finding. `objdump` shows the co-issue arm spilling 42 vector stores and 37
  reloads per iteration: four lanes want 2 state registers each plus 5 live
  schedule registers each, 28 in a file of 16.

Sweeping schedule streams against four fixed round lanes separates registers
from ports:

```
streams   rnds2 M/s   sched M/s   TOTAL M/s   regs
0            4275.7         0.0      4275.7     8
1            4105.9      1070.7      5176.7    13
2            2073.1      1137.5      3210.6    18
3            1661.1      1245.8      2907.0    23
```

One schedule stream is nearly free and pushes total SHA-unit throughput *above*
what the rounds reach alone, so **the issue ports genuinely do have slack** —
the premise Gate C was written to test is confirmed, not refuted. The cliff is
between one stream and two, which is between 13 registers and 18. It is the XMM
file, again, exactly as in Stage 1.

## 6. The one branch not closed, and the arithmetic on it

Stage 2 as specified runs its phases sequentially. The obvious repair is to
**software-pipeline** them: run round phase for round *r* while computing one
lane's schedule for round *r+1*, so the schedule goes back to hiding inside
`rnds2` latency instead of being paid for.

The arithmetic is close enough to be worth stating rather than dismissing. To
clear +15% the risk path needs 1.17×, i.e. a group-round of 0.4663 ns/rnds2.
The round phase costs 0.3058, so the schedule budget is 0.1605 against a
measured 0.2619 — **about 60% of the schedule would have to disappear into the
round phase.**

Probe C says one co-issued stream is free and delivers 1070 M msg-ops/s. Four
lanes need roughly 2157 M/s. So one free stream covers about half — short of
the 60% needed, and a second stream costs 18 registers and halves the round
rate.

**So it is tight-to-negative, and it is genuinely untested.** It is a different
design from the one Phase 1 was asked to measure, and this repo has now twice
watched an untested projection in exactly this margin miss badly. It should be
a probe, not a kernel: extend `bench_phase_split.cpp` with a pipelined variant
and measure the joint. A day, with a hard gate at 1.17×. Not authorised here;
recorded so the next person does not have to re-derive it.

## 7. What still stands

- `sha256rnds2` is latency bound (L=4, T=1) and the shipped two-lane kernel
  runs at roughly half the instruction's sustainable rate. Unchanged, still
  measured, still true.
- The round phase reaches **1.78×** the shipped rate at four lanes. The
  headroom is real and reachable; what is not reachable is keeping it after the
  message schedule stops being free.
- `risk_pool` batching to the back end's real lane width and the epoll work
  both landed and are unaffected by any of this.

## 8. What this closes

The plan's ledger had Stage 2 as the single remaining item with +15% to +45% in
it. It is now closed at +6.2% measured, on the structure that would have
shipped, with the mechanism understood and a written cause.

That leaves Phase 4 — the floor plan — as the live work: LTO/PGO/clang sweep,
fast-path profiling of the 18 µs, spin-then-sleep workers, `vzeroupper` after
round zero. +3–6% combined, no measurement risk, plus the resilience write-up,
which is worth more than any of them and is now considerably richer: two
retractions with controlled diagnostics, three harness bugs caught before they
wrote wrong conclusions into the record, and a measurement discipline that has
killed two plausible designs for the cost of hours each instead of days.

## 9. Reproducing

```
docker build --target build -t obsidio-build starters/cpp
docker run --rm obsidio-build ./build/obsidio-bench-phase-split
docker run --rm obsidio-build ./build/obsidio-stage2-probes
```

Both verify before they time. `bench_phase_split` refuses to print a timing if
any digest fails.

One instrument bug was found and fixed mid-probe, and it is worth knowing about
because it is invisible in the output: GCC's size heuristic left
`schedule_lane` out of line, which added a call per lane per round, forced the
round phase's state to spill around it under the SysV caller-saved rule, and
silently turned the two-lanes-interleaved variant into two sequential calls no
scheduler could overlap. `objdump` caught it — zero `sha256msg1`/`msg2` inside a
function whose entire job is the message schedule. `always_inline` fixes it and
the attribute carries the reason. It moved N=4 from 0.4885 to 0.4747; it did not
change the verdict, but it would have changed the write-up.
