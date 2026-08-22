# The phase-split kernel: the one lead with real score in it

> **CLOSED 2026-08-23. Both stages are now measured and neither pays.**
> Stage 1 was built and retracted at −23% (see the banner in §
> "Stage 1 — widen block 2 only" below). Stage 2 was then built as a verified
> skeleton — the real structure, not a projection — and measures **1.069× on
> the risk path, +6.2% `work_score`**, against a +15% floor.
>
> The mechanism is in [`phase-split-negative-result.md`](phase-split-negative-result.md)
> and it is short: the round phase does reach **1.78×**, exactly as this
> document argued, but phase-splitting converts the message schedule from work
> that hides inside `sha256rnds2` latency into work that has to be paid for. At
> four lanes the schedule is 46% of the group-round, and 1.78× on the other 54%
> does not buy it back.
>
> What survives: `sha256rnds2` is latency bound (L=4, T=1) and the shipped
> kernel runs at about half its sustainable rate. That was true, is still true,
> and is now known not to be convertible by this design. Read the rest as the
> reasoning that was tried.

Measured on `perf/ryzen-ceiling`, 2026-08-23, AMD Ryzen 7 170 / Docker Desktop
/ WSL2. Everything below rests on `obsidio-rnds2-ports` (3 reps) and
`obsidio-bench-chain` from that branch.

**Claim.** `SHA256RNDS2` is latency bound, not port bound. The shipped kernel
runs at roughly half the rate the instruction can sustain, and the risk chain
is ~91% of server CPU. Closing that gap is the only remaining item with a
plausible **+20% to +70%** on `work_score`. It is unbuilt.

---

## 1. What was measured

`obsidio-rnds2-ports` times N independent `sha256rnds2` dependency chains. One
chain is strictly serial, so a single lane can never exceed 1/latency; N
independent lanes are limited only by the instruction's reciprocal throughput.
The headline is a ratio taken inside one run, so laptop clock sag scales both
halves equally.

| lanes | rep 1 | rep 2 | rep 3 | ns/rnds2 (best) |
|---|---|---|---|---|
| 1 | 1.00× | 1.00× | 1.00× | 0.867 |
| 2 | 2.00× | 1.61× | 2.11× | 0.4245 |
| 3 | 2.99× | 2.98× | 3.10× | 0.2897 |
| 4 | 3.05× | 3.56× | 3.14× | 0.2473 |
| 6 | 3.00× | 3.99× | 3.28× | 0.2207 |
| 8 | 3.64× | 3.96× | 3.70× | 0.2223 |

Read carefully, because the benchmark's own headline is unreliable:

- **The printed `lanes to saturate` is argmax over a noisy plateau** — it
  reported 8, 6, 8 across three identical runs. Do not quote it.
- **The reproducible facts** are the 1-lane rate (0.867–0.897 ns, ±1.7% across
  reps) and the 3-lane rate (2.98–3.10×, the tightest point on the curve).
  Scaling clearly continues well past two lanes.
- At ~4.3 GHz, 0.87 ns ≈ **4 cycles latency**; the ~0.22–0.25 ns plateau ≈
  **1 cycle reciprocal throughput**.

That is the whole finding: **L=4, T=1**. Two lanes issue one instruction every
two cycles into a unit that accepts one per cycle.

This settles the question `docs/history/score-levers.md` §4 flagged as
deciding whether a whole branch of work was alive:

> if recip-tput is 2 cycles → x2 already saturates the port. **x3 and x4 gain
> exactly zero, forever.**

**Falsified.** The opposite branch holds.

## 2. What the shipped kernel actually achieves

`obsidio-bench-chain`, same machine, digest verified against the golden before
timing:

| | chains/s | ms/chain | ns/rnds2 |
|---|---|---|---|
| x1 | 241.21 | 4.146 | 1.296 |
| **x2 (shipped)** | **640.46** | **1.561** | **0.4878** |
| x3 | 390.84 | 2.559 | 0.800 |
| x4 | 658.28 | 1.519 | 0.4747 |

One chain-round is two SHA-256 blocks × 32 `rnds2` = 64 `rnds2`; a chain is
50,000 rounds, so 3.2M `rnds2` per chain. That is the conversion that makes
the two benchmarks comparable — everything else is apples to oranges.

Normalised that way the picture is unambiguous:

```
shipped kernel, 2 lanes   0.4878 ns/rnds2
pure rnds2,     2 lanes   0.4245 ns/rnds2   <- kernel is only 15% off at 2 lanes
pure rnds2,     4 lanes   0.2473 ns/rnds2
pure rnds2,     6 lanes   0.2207 ns/rnds2   <- 2.2x below the shipped kernel
```

The kernel is *efficient at the width it runs at*. It is simply running at the
wrong width. And `x4` confirms it: at 0.4747 ns/rnds2 it is statistically the
same as `x2`, because `chain4_impl` is literally two sequential `chain2_impl`
calls — four jobs, still two lanes.

`x3` is worse than `x2` (0.800 vs 0.4878 ns/rnds2, a **39% throughput loss**)
because `chain3_impl` is `chain2 + chain1` and the odd lane runs at the 1-lane
rate. A three-job batch is worse than a two-job batch. That is a live bug, not
just a missed optimisation — see §6.

## 3. Why it is capped at two lanes, and the thing that changes the plan

The cap is a register-budget argument, and it is correct as far as it goes.
Block 1 of each round carries a live message schedule:

- 2 XMM for state (`abef`, `cdgh`)
- 4 XMM for the schedule window (`msg0..msg3`)

= **6 XMM per lane.** With 16 XMM registers, one implicitly consumed by
`sha256rnds2`, two lanes is the honest ceiling. `chain_x86.cpp` says so and is
right.

**But that argument only applies to block 1.** Look at what block 2 already
is (`compress2_const`):

```cpp
inline void compress2_const(State& SA, State& SB) {
  __m128i a0 = SA.abef, a1 = SA.cdgh;
  __m128i b0 = SB.abef, b1 = SB.cdgh;
  for (int i = 0; i < 16; ++i) {
    const __m128i k = KW2V[i];
    a1 = _mm_sha256rnds2_epu32(a1, a0, k);
    ...
```

Block 2 of every steady-state hash is the fixed padding, so its entire message
schedule is a compile-time constant precomputed into `KW2V[]`. It uses **zero**
`sha256msg1`/`msg2`, and costs **2 registers per lane** plus one loaded
constant.

Block 2 is *already in phase-split form*. It is half of all the `rnds2` work in
the chain, it needs no redesign whatsoever, and it is pinned at two lanes for
one reason only: the function it happens to sit inside is a two-lane function.

That converts "rewrite the kernel" into a staged plan whose first stage is
nearly free.

## 4. The staged plan

Per chain-round the work splits evenly: 32 `rnds2` in block 1 (live schedule),
32 in block 2 (constant schedule). Taking the shipped 0.4878 ns/rnds2 average
and the measured pure-`rnds2` plateau:

> **RETRACTED 2026-08-23 — Stage 1 was built and measures −23%.** The full
> result, including the diagnostic that explains it, is in
> `wide-block2-negative-result.md` on `perf/wide-block2`. In short: the loss is
> the cost of holding four chain states across block 1's register-hungry code,
> and widening block 2 recovers nothing net once its inputs are spilled state.
> Stage 2 is not refuted — it separates the phases and never holds N states
> through block 1 — but the premise that Stage 1 is a cheap down payment on it
> is dead, and the projection below missed by 45 points. Read the rest of this
> section as the reasoning that was tried, not as a plan.

### Stage 1 — widen block 2 only

Add `compressN_const` for N = 4 or 6, keeping block 1 as today's pairs. Per
round: run `compress2` over the lanes in pairs, then one wide constant-schedule
pass across all lanes.

```
block 1 stays ~0.55 ns/rnds2   (2 lanes, schedule-bound)
block 2 goes  ~0.22 ns/rnds2   (6 lanes, already 2 regs/lane)
average       ~0.385 ns/rnds2  ->  1.27x on the risk path
```

Risk-path 1.27× against a 91/9 CPU split gives `100 / (91/1.27 + 9)` =
**≈ +24% work_score**. Low risk: no new instruction sequences, no change to
block 1, digests unchanged by construction.

### Stage 2 — phase-split block 1

The real redesign. Compute block 1's 64-word schedule into an L1-resident
buffer first, then run the round phase reading `W+K` from memory at 2 registers
per lane — the same shape block 2 already has. That fits 4–6 lanes.

```
block 1 -> ~0.31 ns/rnds2  (4 lanes + ~15% schedule overhead)
block 2 -> ~0.22 ns/rnds2
average    ~0.265 ns/rnds2  ->  1.84x on the risk path
```

`100 / (91/1.84 + 9)` = **≈ +71% work_score**. Higher risk, and the estimate
is the least trustworthy number in this document — see §5.

## 5. What could make this not pay

Stated plainly, because the upside above is large enough to be worth
disbelieving:

1. **The plateau numbers are noisy.** The 6-lane column ranged 0.2207–0.2895
   across three reps. Stage 2's arithmetic uses the good end. Using the bad end
   drops it to roughly +45%.
2. **The microbenchmark runs bare `rnds2` with no memory traffic.** Stage 2
   adds one 16-byte load per `rnds2`. Loads issue on different ports from the
   SHA unit so this *should* be free, but it is unmeasured. This is the single
   assumption most worth testing before writing any kernel.
3. **Phase-splitting lengthens the per-lane dependency chain** — schedule, then
   rounds, serialised within a lane. It only wins if enough lanes are in flight
   to cover it. Below ~4 lanes it is a regression.
4. **It needs the pool to supply that many concurrent jobs.** Under the graded
   ramp ~170 of 200 VUs sit in the risk queue, so the concurrency exists, but
   `risk_pool` currently batches at most 4 and would need widening in step with
   the kernel.
5. **Wider batches raise per-batch latency.** Risk p95 has 7.3× of margin and
   is queueing-dominated, so this is very likely free — but it is a real
   coupling, not nothing.

There is one piece of positive evidence for the design that came out of a
separate measurement. `bench/x86/rnds2_ports.cpp` reports AVX2 co-issue costing
SHA-NI ~98.5% and concludes the vector ports are saturated. That is an
**AVX/SSE transition artifact**, not port contention — `sha256rnds2` has no VEX
encoding, so mixing it with VEX-256 code pays a dirty-upper penalty every
iteration. Measured directly:

```
                          run1     run2     run3     run4     run5
+ 4x SSE 128-bit adds    -0.0%    -9.9%    -2.7%    +0.4%   -10.8%
+ 4x AVX2 256-bit adds  -98.5%   -98.5%   -98.6%   -98.6%   -98.5%
+ 4x AVX2 + vzeroupper  -99.3%   -99.3%       —        —        —
```

**Four 128-bit vector ops per `rnds2` cost between 0% and 11%; four 256-bit
ops cost 98.5%.** The SSE arm is noisy and bimodal across runs, so it is not
free — but the two orders of magnitude between the arms is not in question,
and the AVX2 figure is stable to ±0.1%.

The message schedule costs roughly 1.5 vector ops per `rnds2`, less than half
the probe's four, so there is spare vector issue capacity to run it alongside
the rounds — provided everything stays 128-bit. Budget for it costing
something rather than nothing. This is also why the kernel must not reach for
AVX2.

## 6. The cheap fix to do regardless

Independent of either stage, `x3` being 39% slower than `x2` is a live defect:
`risk_pool` hands the backend three jobs whenever three are queued, and the
third runs at half rate. The fix is not to hardcode 2 — `chain_arm.cpp` is a
genuine four-lane interleave and would be halved by that. Give `Backend` a
lane-width field and have the pool batch to `backend->lanes`. Small, portable,
provably not worse, and it is exactly the seam Stage 1 needs to exist anyway.

## 7. What this supersedes

- `docs/history/score-levers.md` §4 — the "x3/x4 dead forever" branch is
  falsified; the ceiling analysis built on it is live again.
- `docs/history/score-levers.md` §3 — "close the ~20% gap to the two-lane
  floor" is aiming at the wrong target. The kernel is within 15% of the *two-
  lane* floor already. The gap worth chasing is the lane count, not the
  two-lane efficiency.
- `HOW-TO-TEST.md` step 1's port-contention verdict — "the hybrid stays dead"
  is not supported by that measurement.
- The `RISK_SCHED` sweep is closed negative and unrelated: all six scheduling
  classes landed within noise, with container CPU at ~198% in every arm. There
  is no stranded capacity, so nothing there competes with this.

## 8. Order of work

1. Extend `rnds2_ports.cpp` with a memory-sourced `W+K` variant and a
   `msg1`/`msg2` co-issue arm. ~1 hour, and it de-risks §5.2 and §5.3, the two
   assumptions Stage 2 rests on.
2. Backend lane-width field + pool batching to it (§6). Small and independent.
3. Stage 1: `compressN_const`. Measure against `x2` on the same box.
4. Stage 2 only if 1–3 hold up.

*Nothing in §4 is implemented. Every figure is one laptop under Docker Desktop
/ WSL2; the reasoning is portable, the absolute numbers are not.*
