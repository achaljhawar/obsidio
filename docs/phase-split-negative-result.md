# The phase-split kernel: built as a skeleton, measured, and it does not pay

**Result: +6.2% projected `work_score` against a +15% floor. Gate A failed.
Stage 2 as specified is closed. `src/` is untouched — nothing was shipped, and
nothing needed to be.**

> **Sequel, same day: the pipelined variant of §6 was authorised, built and
> measured, and it PASSES at 1.413× — about +36%.** The sequential phase-split
> below is still dead for the reason given; what revives the design is
> co-issuing the schedule again, across lane groups, at eight chains wide. See
> [§6a](#6a-outcome--the-probe-was-run-and-it-passes). Still nothing
> integrated: kernel work is a separate authorisation, and the open questions
> at the end of §6a are real.

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

> **Corrected 2026-08-23, in review.** The first version of this section said
> "about 60% of the schedule would have to disappear" and concluded
> *tight-to-negative*. That inverted a ratio: 0.1605/0.2619 = 61% is the
> fraction of the schedule that may **survive exposed**, so the fraction that
> must disappear is 39%, not 60%. A second error sat underneath it — the
> `stage2_probes` requirement line assumed one msg op per block-1 `rnds2` when
> the real sequence has 0.75, overstating the requirement by 4/3. Both are
> fixed below. The corrected arithmetic points the other way: this branch is
> **tight-to-positive**, and it is the highest-expected-value work left.

Stage 2 as specified runs its phases sequentially. The obvious repair is to
**software-pipeline** them: run the round phase for round *r* while computing
one lane's schedule for round *r+1*, so the schedule goes back to hiding inside
`rnds2` latency instead of being paid for.

**The op accounting**, since everything below depends on it.
`compress_generic`'s schedule is **12 `sha256msg1` + 12 `sha256msg2` per block
1**, and block 2 has none. That is 24 SHA-unit ops against block 1's 32
`rnds2`, so over a whole 64-`rnds2` round it is **0.375 msg-ops per `rnds2`**.
Confirmed by `objdump` on `bench_phase_split`'s inlined `schedule_lane`
(`palignr` cross-checks at 12 per copy). The other 24 schedule ops per block —
`alignr` and `add` — are ordinary vector ops on other ports.

**The budget.** To clear +15% the risk path needs 1.17×, i.e. a group-round of
0.5456/1.17 = **0.4663 ns/rnds2**. Co-issuing one schedule stream costs the
round phase 4.1% in the canonical Probe C run (4275.7 → 4105.9 M rnds2/s; the
range across three runs is 0% to 7.8%), so the round phase goes 0.3058 →
**0.3184**. That leaves **0.1479** for exposed schedule against a measured
0.2619, so the fraction that must be hidden is

    1 − 0.1479/0.2619 = 43.5%

**What one stream actually hides.** A round phase at 0.3184 ns/rnds2 runs at
3140 M rnds2/s, which demands 0.375 × 3140 = **1178 M msg-ops/s**. One
co-issued stream supplies **1070.7 M/s** — about **91%**, not the ~50% the
first version claimed off the inflated requirement. Carrying the residual
through: 9% of 0.2619 = 0.024 ns exposed, total ≈ **0.342 ns/rnds2**,
r ≈ **1.59×**, ≈ **+51% `work_score`**.

**And that is the number to distrust.** It lands above the +45% the plan called
its realistic ceiling, off a single 13-register measurement extrapolated onto a
structure that has never been built. The bounds are what matter:

| if the pipelined round phase… | round ns | total | r | score |
|---|---|---|---|---|
| absorbs the schedule entirely | 0.3184 | 0.318 | 1.71× | +61% |
| behaves as the model above | 0.3184 | 0.342 | 1.59× | +51% |
| hits the streams=2 register cliff | 0.6309 | ~0.65 | 0.84× | **−15%** |

**The whole question is whether the register file holds**, and nothing measured
so far answers it. Probe C's streams=1 arm fits in 13 registers because its
round phase is a toy: four lanes of 2 state registers with the round constant
in one shared register. Stage 2's *real* round phase additionally needs a
per-lane `msg` temporary loaded from the W+K buffer and the block-1
feed-forward values live across the block-1→block-2 boundary. Adding a
5-register schedule stream to *that* is the untested step, and the streams=2
row shows what crossing the file costs: the round rate halves and the design
lands below the shipped kernel.

Transplanting Probe C's light-round-phase co-issue number onto Probe A's heavy
round phase is exactly the error that killed Stage 1 at −23% and Stage 2 at
+6.2%: joining two separately-measured halves across the register boundary they
have to share. The arithmetic above is that error, written out deliberately so
it can be seen.

**So the conclusion is unchanged and the reason for it is stronger.** This is a
probe, never a kernel: extend `bench_phase_split.cpp` with a pipelined variant
and measure the joint, digests first. A day, hard gate at 1.17×. What has
changed is its priority — a band of −15% to +61% on one day of measurement is
the best expected value left on the table, well ahead of Phase 4's mechanical
+3–6%. Not authorised here; recorded so the next person does not re-derive it,
and does not re-derive it wrong.

### 6a. Outcome — the probe was run, and it passes

**Gate P: PASS at 1.371× worst across four clean runs, 1.413× on a cool box —
roughly +33% to +36% `work_score`.** Nothing was integrated; `src/` is
untouched. Kernel work remains a separate authorisation.

**The design had to change, and the reason is worth more than the number.** You
cannot hide round *r+1*'s schedule inside round *r*'s round phase for the same
lane — the schedule's input is the hex that round phase has not produced yet.
Within a lane the dependency is strictly serial and no scheduling gets around
it. So the pipeline runs across lane **groups**: split N lanes in half, prime
one half's schedule, then alternate `rounds(A) ‖ schedule(B)` with
`rounds(B) ‖ schedule(A′)`. Two steps advance every lane one round.

Cool box, worst rep of 7, shipped x2 at 0.5200 ns/rnds2 in the same process:

| structure | N | ns worst | vs shipped |
|---|---|---|---|
| rounds ×1 ‖ schedule ×1 | 2 | 0.7654 | 0.679× |
| rounds ×2 ‖ schedule ×2 | 4 | 0.4423 | 1.176× |
| rounds ×3 ‖ schedule ×3 | 6 | 0.4356 | 1.194× |
| **rounds ×4 ‖ schedule ×4** | **8** | **0.3679** | **1.413×** |

Attribution is clean, because the sequential variant sits in the same binary at
the same round-phase width: sequential 4-lane phase-split measures 0.4689 and
the pipelined 4-lane round group measures 0.3679. **Co-issuing the schedule is
worth 27% with everything else held fixed.**

**Where it landed in the bounds table: between the gate and the model.** Not
the −15% register cliff, not the +51% model, but a solid +33–36%.

**The register file held — the open question is answered, and by construction
rather than by luck.** Probe C's streams=2 catastrophe never reproduces here
because the pipeline is arranged so that at most **one** schedule stream is
live at any moment. That was the whole point of dealing the schedule out step
by step instead of running N streams at once.

**Why it fell short of +51%**, since the gap is instructive:

1. The same-lane dependency forces group pipelining, so the round phase is only
   N/2 lanes wide. The model assumed a full-width 4-lane round phase with a
   stream beside it; that configuration does not exist.
2. `objdump` shows that at HALF ≥ 3 only **two** lanes' schedules are actually
   co-issued — lane 0 rides block 1, lane 1 rides block 2, and lanes 2+ fall
   through to a plain `schedule_lane` loop, because a 16-pair block has nowhere
   else to deal them. So 1.413× is achieved with roughly **half** the schedule
   hidden, not the ~91% the model assumed.
3. Reaching it needs **eight** chains in lockstep, not four.

Point 2 means there may be more here, or there may be a register wall one step
further on. Untested, and deliberately so.

**Open questions before any of this becomes a kernel** — none of them
authorised, all of them cheap to state:

- **The pool would have to batch 8.** `Backend::lanes` is 2 and
  `risk_pool.cpp` still has a hardcoded `RiskJob jobs[4]`. That is a wider
  change than the plan's Phase 2 anticipated, and the drain-mode composition
  rules for odd remainders get correspondingly more awkward.
- **Per-batch latency roughly triples.** An 8-chain lockstep batch is
  0.3679 ns × 3.2M × 8 ≈ **9.4 ms** of compute against the shipped 2-chain
  batch's ≈ 3.3 ms. `{tier:risk}` p95 is 203 ms against a 1500 ms bar and is
  queueing-dominated, so ~6 ms of extra service time should vanish — but "should"
  is the word that has cost this project two designs, and Phase 3 is where it
  gets checked.
- **The skeleton is not a kernel.** It omits `first_round`, the string
  handling, and the pool's job plumbing. The comparison is fair because the
  shipped arm is measured the same way, but integration losses are real and
  historically unkind.

## 7. What still stands

- `sha256rnds2` is latency bound (L=4, T=1) and the shipped two-lane kernel
  runs at roughly half the instruction's sustainable rate. Unchanged, still
  measured, still true.
- The round phase reaches **1.78×** the shipped rate at four lanes. The
  headroom is real and reachable; what is not reachable is keeping it after the
  message schedule stops being free.
- `risk_pool` batching to the back end's real lane width and the epoll work
  both landed and are unaffected by any of this.

## 8. What this closes, and what it opens

The plan's ledger had Stage 2 as the single remaining item with +15% to +45% in
it. **Sequential** phase-split is closed at +6.2% measured, on the structure
that would have shipped, with the mechanism understood and a written cause.

The **pipelined** structure of §6a is not closed — it clears the gate at 1.413×
and is now the highest-value item on the board by a wide margin. What it needs
next is not more bench work but a decision: it implies an eight-wide risk
batch, a `Backend::lanes` and `risk_pool` widening beyond anything the plan
scoped, and a latency coupling that has to be validated rather than assumed.
That is a Phase 2 authorisation, and it should be taken deliberately — the last
two designs that looked this good at bench level cost 23 points and a gate
respectively.

Behind it, Phase 4, the floor plan: LTO/PGO/clang sweep, fast-path profiling of
the 18 µs, spin-then-sleep workers, `vzeroupper` after round zero. +3–6%
combined, no measurement risk, plus the resilience write-up,
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
