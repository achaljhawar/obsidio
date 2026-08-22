# Widening block 2 to four lanes: measured, and it does not work

**Result: −23% on the risk path. Not shipped. `src/` on this branch is
identical to `main`.**

This retracts "Stage 1" of `docs/phase-split-kernel.md` (present on
`perf/ryzen-ceiling`), which rated it ≈ +24% and called it the low-risk half of
the phase-split plan. The reasoning there was sound and the supporting
measurements were real; the conclusion was still wrong, for a reason none of
those measurements could see.

Measured 2026-08-23, Ryzen 7 170 / Docker Desktop / WSL2.

---

## 1. The idea, and why it looked good

Every steady-state hash in the chain is two SHA-256 blocks. Block 1 carries a
live message schedule and needs ~6 XMM per lane (2 state + 4 schedule window),
which against 15 usable registers is what caps the kernel at two lanes. Block 2
is the fixed padding, so its schedule is the compile-time `KW2V[]` table:
`compress2_const` is pure `rnds2` plus one load and one shuffle per four
rounds, and costs **2 registers per lane**.

So block 2 was already in "phase-split" form and was pinned at two lanes only
because the function enclosing it was two-lane. Widening just that half looked
like a free win requiring no new instruction sequences.

Measured in isolation, block 2 scales exactly as that argument predicts
(median of 7, `Mrnds2/s`):

| lanes | 1 | 2 | 3 | **4** | 6 | 8 |
|---|---|---|---|---|---|---|
| | 1159 | 2358 | 3366 | **4361** | 4013 | 3057 |

Four lanes is the peak — **1.85× the two-lane rate** — and it is also exactly
what the pool already batches. Splitting the shipped kernel's 62.44 ns
two-lane group-round using that number gives block 1 = 35.30 ns and block 2 =
27.14 ns, which projects a 1.249× risk path and **+22.2% `work_score`**.

## 2. What actually happened

`chain4_impl` was rewritten to interleave at the round level: block 1 as two
sequential `compress2` calls, then one `compress4_const` across all four
resulting states. Digests verified — `verify_lane4()`, the forced
`RISK_BACKEND=x86-sha-ni` selftest, and a direct four-chain comparison all
agree byte for byte.

Both paths timed in one process, alternated, through the public API (two
`risk_hash_x2` calls *are* the old `chain4` exactly):

```
  two chain2 groups (old chain4)         0.60 ms    6668.07 chains/s
  one chain4 (wide block 2)              0.78 ms    5128.51 chains/s

  risk path 0.769x  ->  projected work_score -21.5%
```

## 3. Why — the diagnostic that matters

The obvious suspect is register pressure, but that is a guess until it is
separated from the block-2 width. So: keep the new structure — all four chain
states live across both `compress2` calls — but do block 2 as **two 2-lane
passes** instead of one 4-lane pass. If widening block 2 were paying for
itself, this should be clearly worse than the 4-lane version.

```
  narrow block 2, 4 states live          risk path 0.767x
  wide   block 2, 4 states live          risk path 0.769x
```

Identical. Two conclusions follow, and the second is the important one:

1. **The entire −23% comes from holding four chain states across block 1.**
   `compress2` needs 12–14 registers for its own two lanes; the two states
   waiting their turn get spilled, and the spill lands *inside* a 64-round
   schedule loop.
2. **Widening block 2 gains nothing net — not "less than hoped", nothing.**
   The isolated 1.85× is entirely consumed. Block 2's inputs are now spilled
   state that must be reloaded, so its lane width stopped being the constraint.

The projection's error was treating block 1 and block 2 as separable when the
chain state has to be carried across the boundary. Every input to the +22.2%
figure was correctly measured; the model connecting them was wrong.

## 4. What this does and does not say about Stage 2

**Does not refute it.** Stage 2 is a different structure: compute block 1's
message schedule into an L1 buffer, then run the round phase at 2 registers per
lane. Its schedule phase and round phase are separate, each with modest
register pressure, so it does not hold N states through register-hungry code —
which is the specific thing that failed here.

**Does refute the plan's shape.** Stage 1 was the cheap, low-risk down payment
that made Stage 2 worth starting. There is no cheap half. Anyone picking this
up is choosing to do the whole redesign on the strength of a microbenchmark,
having just watched a microbenchmark-derived projection miss by 45 points.

Concretely, before writing Stage 2, measure the thing that killed Stage 1:
**how much does it cost to keep N chain states live across a phase boundary?**
That number, not `rnds2` throughput, is what decides the design. It was never
measured because nothing suggested it mattered.

## 5. Reproducing

`bench/x86/bench_wide_block2.cpp` is retained and builds via the
`obsidio-bench-wide-block2` target. Against unmodified `src/` it compares two
identical paths and should report ≈ 1.0× — that is the control. To re-run the
experiment, replace `chain4_impl` in `chain_x86.cpp` with a round-level
interleave and add:

```cpp
inline void compress4_const(State& SA, State& SB, State& SC, State& SD) {
  __m128i a0 = SA.abef, a1 = SA.cdgh;   // ... b, c, d likewise
  for (int i = 0; i < 16; ++i) {
    const __m128i k = KW2V[i];
    a1 = _mm_sha256rnds2_epu32(a1, a0, k);   // ... all four lanes
    const __m128i ks = _mm_shuffle_epi32(k, 0x0E);
    a0 = _mm_sha256rnds2_epu32(a0, a1, ks);  // ... all four lanes
  }
  SA.abef = _mm_add_epi32(a0, SA.abef);      // ... all four lanes
}
```

then have `chain4_impl` run `compress2(sa,ma,sb,mb)`,
`compress2(sc,mc,sd,md)`, `compress4_const(sa,sb,sc,sd)` per round.

The bench builds outside the image: `bench/` is not copied into the Docker
build context on this branch, so the `EXISTS` guard in `CMakeLists.txt` skips
the target there. Build it by mounting the source instead.

## 6. Still standing

Nothing else in `phase-split-kernel.md` is affected by this. In particular the
finding that `sha256rnds2` is latency bound (L=4, T=1) and that the shipped
kernel runs at roughly half the instruction's rate is unchanged and still
measured — what is now in doubt is whether any structure can convert that
headroom into throughput without paying it back in state traffic.
