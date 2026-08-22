# Obsidio — Session Findings

> **Historical snapshot (2026-08-22):** this is the final measurement record for
> the native x86 session, not the current project guide. Repository-cleanup
> recommendations in §§12–13 were acted on selectively after this document was
> written. See the [project README](../../README.md) for current status and the
> [C++ guide](../../starters/cpp/README.md) for current operation.

> References to the former `AUDIT-HANDOFF.md` and old Compose template are
> retained as historical evidence. Both were removed during repository cleanup
> and remain available through Git history.

**Date:** 2026-08-22
**Machine:** AMD Ryzen 7 170 (8C/16T, x86-64), Windows 11, Docker Desktop / WSL2
**Repo at start:** `0de6afe` · **at end:** `8beeacd`
**Supersedes parts of:** [x86-coarse-audit.md](x86-coarse-audit.md) (see §11 — that document contains claims this session disproved)

Everything below was measured in this session unless attributed to a repo document. Where a conclusion was drawn and later overturned, both are recorded — the corrections turned out to be as informative as the results.

---

## 1. Headline

| | |
|---|---|
| **Biggest win** | The x86 hash kernel was rewritten as a register-resident two-lane interleave: **2.389×** on the risk path, measured. |
| **Biggest surprise** | The grading architecture is **x86-64**, not the arm64 Mac the [ARM strategy](arm64-strategy-notes.md) asserted at the time. |
| **Biggest trap avoided** | The AVX2 hybrid idea was **tested and killed** before anyone wrote 600 lines of it. |
| **Biggest methodological finding** | This laptop **thermally throttles to 1.49 GHz**, halving scores over a session. Absolute numbers from it are near-worthless; only alternated A/B ratios survive. |
| **Still unclaimed** | The persistence bonus scores **zero** as shipped. |

---

## 2. The architecture question, settled by hardware

The [ARM strategy](arm64-strategy-notes.md#6-which-machine-for-which-work) states *"the grading box is an arm64 Mac"*, and its x86 section declares *"the x86 question is closed. No x86 back end."*

The machine is an **AMD Ryzen 7 170**, x86-64, with `sha_ni`, `avx2`, `bmi2`, `adx`, `vaes` (no AVX-512). Docker runs `linux/amd64`.

Consequences:
- `chain_arm.cpp` is dead code at grading time.
- Every headline in the [ARM strategy](arm64-strategy-notes.md) §5 — the 2.96×, the +36%, the x4 lane choice, the 132 ms `/risk` p95 — describes silicon that will not run the graded workload.
- The x86 SHA-NI back end, which that document calls "dead", is the code that actually serves.

**The then-current ARM strategy instructed a reader to make the wrong decision.** This nearly happened in-session. It is now explicitly labelled as superseded history.

---

## 3. First contact: the x86 back end had never run

The former `AUDIT-HANDOFF.md` §0.3 stated: *"No end-to-end measurement of the x86 path exists in this repo."* The [ARM strategy](arm64-strategy-notes.md) records that an amd64 cross-build on the Spark died at `exec format error`.

This session ran it for the first time.

- Image built; both selftest gates passed (63 assertions, not the 56 the ARM strategy claimed at the time).
- Startup banner: `hash back end: x86-sha-ni (x1..x4 coarse interleave)` — CPUID gate passed, per-lane boot verification passed.
- Digest for `seed=0.5` verified against an independent Python reference in a clean container: **exact match**.
- Also confirmed at startup: `persistence: DISABLED (/data/prices.log not writable)` — exactly as the audit predicted.

**First graded run:** `work_score` **1,901,672**, all four thresholds green, 0.00% errors across 761,360 requests. Container pegged at 199–202% CPU (its exact 2-core quota), 5.1 MiB of 2 GiB.

---

## 4. The measurement crisis

### 4.1 A wrong conclusion, drawn and retracted

An A/B against the reference fallback produced:

| run | backend | `work_score` |
|---|---|---|
| 1 | x86-sha-ni | 1,901,672 |
| 2 | reference | 2,108,026 |
| 3 | **x86-sha-ni (identical to run 1)** | **2,667,160** |

Conclusion drawn: *"the reference fallback beats the x86 backend by +10.9%; `chain_x86.cpp` is harmful."*

**Retracted.** Run 3 was the same binary as run 1 and scored **40.25% higher**. The three runs climbed monotonically in wall-clock order. A 40% spread cannot resolve a 10.9% difference.

### 4.2 Building an instrument that works

A 40-second, risk-only, 16-VU probe was written. Alternating arms (A/B/A/B/A/B) within one scripted sequence so time-drift cancels:

| | rep 1 | rep 2 | rep 3 | mean | spread |
|---|---|---|---|---|---|
| x86-sha-ni | 457.23 | 462.42 | 460.57 | **460.07 /s** | 1.13% |
| reference | 358.88 | 357.34 | 357.78 | **358.00 /s** | 0.43% |

**The x86 back end is worth +28.5%**, at ~1% resolution, every rep of one arm beating every rep of the other. The former handoff's §0.3 question was closed: the back end earns its place, and the audit's pessimism about it was wrong.

**Lesson: the full 4m30s graded script is not an A/B instrument on this machine. The 40s alternated probe is.**

---

## 5. The optimisation: fused round-by-round x2

### 5.1 What was wrong

`chain2/3/4` called `hash64()` once per lane per round:

```cpp
for (int r = 0; r < rounds; ++r) {
  hash64(sa, sa);   // state round-trips through char[64] every iteration
  hash64(sb, sb);   // sequential; no register-level overlap
}
```

Structurally **x1 with memory traffic**. Two independent costs:

1. `sha256rnds2` has ~4-cycle latency against ~1-per-2-cycle throughput, so one serial chain leaves the SHA port mostly idle.
2. `hex_encode()` (`sha256.cpp:172`) is a **scalar byte loop** — 64 table lookups and 64 stores per hash, 50,000 times per request.

### 5.2 What replaced it

- `block1_x2` / `block2_x2` — both lanes advanced in lockstep, states and message schedules resident in XMM for all 50,000 rounds. Per-lane instruction sequence identical to the verified `compress_generic`; only scheduling differs.
- `hex16` — SIMD hex, two `pshufb` per 16 bytes, writing straight into the next round's message registers.
- **Nothing round-trips through memory.** `kBswap` is an involution, so the final ASCII is recovered from the message vectors with one shuffle at the end rather than stored every iteration.
- `chain3` = fused pair + single; `chain4` = two fused pairs.

### 5.3 Why two lanes and not four

| | per lane | ×2 | ×3 |
|---|---|---|---|
| state (ABEF, CDGH) | 2 | 4 | 6 |
| message schedule | 4 | 8 | 12 |
| scratch | — | 2 | 2 |
| XMM0, reserved by `sha256rnds2`'s implicit operand | — | 1 | 1 |
| **total of 16** | 6 | **15** ✅ | **21** ❌ |

ARM has 32 NEON registers and no implicit reservation, which is why `chain_arm.cpp` reaches four lanes. **The +36% four-lane win is physically unavailable on x86.**

### 5.4 Result

Alternating probe, completed requests in 40s:

| | rep 1 | rep 2 | rep 3 | mean | spread |
|---|---|---|---|---|---|
| fused x2 | 43,522 | 44,171 | 43,904 | **43,866** | 1.48% |
| coarse | 18,350 | 18,394 | 18,333 | **18,359** | 0.33% |

**2.389×** → 4.347 → **~1.82 CPU-ms per chain**, landing on the two-lane floor implied by 64 `rnds2` per iteration at ~4-cycle latency.

Full graded runs (cool machine):

| | run 1 | run 2 |
|---|---|---|
| `work_score` | **5,811,626** | **5,752,161** |
| requests | 2,325,120 | 2,302,078 |
| `/risk` p95 | 205.87 ms | 203.45 ms |
| `/price` p95 | 336.32 µs | 345.44 µs |
| `/stats` p95 | 338.58 µs | 348.29 µs |
| errors | 0.00% | 0.00% |

### 5.5 Correctness

- 63 build-gated assertions pass with the fused kernel selected, including cross-lane agreement (x4 vs x3 vs x2 vs x1)
- Digests for `seed=0.5` and `seed=0.4821` verified against Python, out of the running container
- **ASan + UBSan clean**
- Both Docker gates pass (default and `RISK_BACKEND=reference`)

Committed to `perf/x86-fused-x2-interleave` (`71863dd`), pushed to origin.

---

## 6. Independent convergence

While the above was in progress, **PR #2 (`600220f`, Pratham Nayak) merged the same optimisation to main** — register-resident x2, SIMD hex, x3 as pair+single, x4 as two pairs. Two people solved the same problem the same way within hours.

Main's version was verified only *"under a scalar emulation of the SHA-NI/SSE intrinsics, since no arm64 dev machine can execute them natively"* — never run on real x86. This session ran it.

Head-to-head, both with alternated designs:

| Measurement | main (`600220f`) | branch (`71863dd`) | delta |
|---|---|---|---|
| Risk-only probe, 3 alternating reps | 43,971 | 44,510 | +1.23% |
| Full graded, clean back-to-back | 2,603,124 | 2,656,259 | +2.04% |

**A tie** — 1–2% sits inside main's own 1.69% spread. No architectural difference.

Main is ahead on one thing that matters and is orthogonal to the kernel: its Dockerfile runs **four** selftest passes including a forced `RISK_BACKEND=x86-sha-ni`, so a verification failure becomes a build failure instead of a silent ~28% drop to the fallback. That fixes audit §15.4 / recommendation 7.

**Recommendation: keep main's kernel, delete the branch.** Its value was the evidence, which transfers since the implementations are equivalent.

---

## 7. The AVX2 hybrid: tested and killed

### 7.1 The idea

`sha256rnds2` issues on one specific execution port. If two lanes saturate it, Zen's other vector pipes idle. An AVX2 8-way multi-buffer SHA-256 uses ordinary vector integer ops on *those* pipes — so run concurrently, it might add throughput for free. Modelled at ~1.45× beyond the SHA-NI ceiling, which would have put ~8.9M in play.

### 7.2 Two broken benchmarks before a valid one

- **v1** held 8 `__m128i` + 6 `__m256i` live. **YMM0–15 alias XMM0–15** — 14 of 16 plus constants, so it spilled every iteration and measured spill traffic. Its clock loop was also miscounted (a 3-cycle chain divided by 2).
- **v2** fixed the register budget to 9 and the collapse persisted, ruling out spilling. Its clock loop (`x += i`) was folded to a closed form by GCC, reporting "5,747,126 GHz".
- **v3** added an `asm volatile` barrier to the clock chain and ran the mix at two vector widths to isolate the mechanism.

### 7.3 The verdict

```
core clock (measured)      : 1.49 GHz

sha256rnds2 solo           : 3.3855e+08 /s   (4.39 cycles each)
avx2-256 round solo        : 4.4088e+08 /s   (3.37 cycles each)
avx-128 round solo         : 4.8011e+08 /s   (3.09 cycles each)

MIX sha + avx2-256         : sha at   2.3% of solo, avx at  1.7%  -> U = 0.040
MIX sha + avx-128 (VEX)    : sha at 135.5% of solo, avx at 95.6%  -> U = 2.311
```

Identical work, identical registers, identical loop — **only the vector width differs, and it costs 60×.**

That is the **AVX–SSE transition penalty**. `sha256rnds2` has no VEX encoding, so it is a legacy-SSE instruction; interleaving it with 256-bit VEX leaves dirty upper-YMM state that stalls on every switch.

**Why this kills the idea specifically:** a multi-buffer only beats SHA-NI at 8 lanes / 256-bit. At 128-bit you get 4 lanes — roughly 376 cycles per chain-iteration against SHA-NI's ~281, i.e. slower. **The only width that could win is the one that cannot coexist with SHA-NI.** Phase-separating them (with `vzeroupper` between blocks) pays the penalty once instead of per-instruction but destroys the overlap that was the whole point.

**The ~8.9M tier is off the table.** An hour of benchmarking saved days of the hardest code in the project.

**Caveat:** the SHA-NI solo baseline used only 3 independent chains and was latency-bound, not port-bound — hence the mixed 128-bit case showing SHA at 135% of "solo". So `4.39 cycles per sha256rnds2` is an upper bound on throughput cost, not the verified port limit. The 256-vs-128 comparison is internally controlled and unaffected.

---

## 8. Thermal throttling — the finding that reframes everything

The same image, same command, same machine:

| | early (cool) | late (hot) | ratio |
|---|---|---|---|
| `work_score` | 5,781,894 | 2,656,259 | 2.18× |
| `/risk` p95 | 205 ms | 440 ms | 2.15× |
| `/price` p95 | 336 µs | 658 µs | 1.96× |
| **core clock** | ~4 GHz nominal | **1.49 GHz measured** | 2.68× |

Everything degraded by the same factor **including `/price`**, which never touches the hash chain. That is a clock drop, not a code regression.

**Implications:**

1. **No absolute number from this box is trustworthy** without recording how long it had been under load.
2. **The ratios survive.** Every optimisation conclusion here came from tightly alternated A/B, where both arms see the same thermal state. That design choice is what makes the 2.389× defensible.
3. 1.49 GHz is low even for a throttled laptop — **check the Windows power plan and whether the machine is on battery.** If the CPU is being capped by policy rather than heat, every measurement including the "cool" 5.8M is understated.

A cooldown experiment (15 min idle, re-measure clock, re-run graded) was in flight at the time of writing. Hot reading with containers stopped had already recovered 1.49 → 1.65 GHz.

---

## 9. Ceiling analysis

```
score = 2.5 × requests           (fixed by the client's 60/30/10 mix)
requests bounded by CPU; /risk is ~91% of the cost
```

Validated against three runs — `requests × 2.5` predicted `work_score` to within **0.02–0.09%** on hardware the model was never derived on.

| Tier | Ceiling | Status |
|---|---|---|
| Current (cool) | ~5.8M | measured |
| SHA-NI port limit — close the last ~12% via asymmetric x3 | **~6.4M** | the realistic ceiling |
| AVX2 hybrid | ~8.9M | **DEAD** (§7) |
| Chain free (fast path only: 79,080 req/s at 153% CPU) | ~53M | proves HTTP is nowhere near binding |

The asymmetric x3 is the one remaining kernel idea: block 2's message schedule is a compile-time constant, so a third lane needs **no schedule registers there** and spills only during block 1. Uncertain, maybe half of the remaining 12%.

**Definitively closed:** more threads (2-CPU cap, pegged at 202%), caching (random seed; `/stats` forbidden; `/price` already a map), load shedding (1% error ceiling), GPU (none, and the chain is sequential), fast-path work (~3% available even if halved).

---

## 10. Comparison with the Spark, decomposed

Before this session's optimisation:

| Factor | Ratio |
|---|---|
| Hardware + WSL2 | 1.24× |
| Kernel (ARM x4 vs x86 coarse) | 2.30× |
| **Product** | **2.86×** (observed 2.856×) |

After: the kernel gap closed from 2.30× to roughly **1.18×** — 1.797 ms/chain against the Spark's 1.522. **The remaining difference is mostly hardware**: a Cortex-X925 at 3.9 GHz on native Linux versus a throttling laptop Ryzen in a WSL2 VM.

**8,866,401 is a hardware number, not a code number.** If grading runs on hardware like this, no team reaches it.

---

## 11. Corrections issued during this session

Recorded rather than deleted, because the reasons they were wrong are themselves findings.

| Claim | Status | Why |
|---|---|---|
| "Disabling the x86 backend is worth +10.9%" | **Retracted** | Measurement noise; correct sign is **+28.5% in favour** |
| "The 40% spread was machine warm-up" | **Retracted** | It is thermal throttling, and worse than 40% |
| "Your score is 5.78M" | **Qualified** | True cold; the same image scores 2.66M hot |
| "main's `/price` being 2× slower means its kernel differs" | **Retracted** | Both images degraded identically; it was thermal |
| "The 2.6M run was contaminated by my `docker stats` sampling" | **Retracted** | A clean re-run reproduced it |
| "Build AVX2 multi-buffer" (implied by the ARM strategy roadmap) | **Refuted** | §7 — transition penalty makes it impossible |

The earlier [x86 coarse audit](x86-coarse-audit.md) was written before items 2, 3 and 6 were known and **contains the superseded 40%-noise-as-warm-up claim**. This document supplies the correction.

---

## 12. Repository defects recorded at session end

Current resolution of this dated list:

| Item | Current status |
| --- | --- |
| Persistence not enabled by the image | **Open.** File-log support exists, but the image still needs an explicit writable `/data` mount. Durability error handling also needs work. |
| Incorrect Node/Postgres Compose file | **Resolved.** Removed; no replacement is claimed. |
| Current docs asserted ARM grading | **Resolved.** The ARM document is now labelled as superseded history and current guides describe runtime dispatch. |
| `IO_THREADS=1` not retested on x86 | **Open.** Requires a controlled mixed-workload experiment. |
| Generated build artifacts tracked | **Resolved.** Removed and ignored. |
| Current self-test count stale | **Resolved.** Current guides intentionally avoid a hard-coded count. |
| ARM-only chain microbenchmark | **Open.** It remains experimental and is not a default build target. |
| Required resilience write-up missing | **Resolved.** Added as `docs/resilience-writeup.md`. |

The original snapshot follows unchanged for auditability.

| # | Item | Evidence |
|---|---|---|
| 1 | **Persistence bonus scores zero.** Dockerfile never creates `/data`, declares no `VOLUME`; runtime logs `persistence: DISABLED` on every start | observed on every run |
| 2 | **The former Compose file described a system nobody built** — it built the deleted Node starter with Postgres | historical lines 18, 24, 32, 37–38 |
| 3 | **The ARM strategy asserted an arm64 grading box and a dead x86 path** | historical sections 6, 7, 9, and 11 |
| 4 | **`IO_THREADS=1` had not been retested on x86** (+3.6–3.9% on ARM); needed a mixed-workload probe | ARM strategy thread sweep |
| 5 | **Build artifacts tracked in git** — `.gitignore` lists them but they were committed in the same push, so it does not apply | ~110 files, incl. arm64 `bench/throttle` |
| 6 | **Selftest count stale** — docs say 56, actual 63 | `grep -c expect_eq` |
| 7 | **`bench/bench_chain.cpp` is ARM-only** (`#include <arm_neon.h>`) — no x86 microbenchmark exists | line 11 |

**Fixed during this session by PR #2:** forced-backend build gates (audit §15.4) — the Dockerfile now runs four passes.

---

## 13. Recommendations at session end

This list records the priorities at the time. It is not the current work queue;
consult the root README's known-limitations section before acting on it.

1. **Claim the persistence bonus** (~20 min). `mkdir -p /data` + `VOLUME` in the Dockerfile, add a C++-specific Compose file with a named volume, and verify with `tests/persist_test.sh`. `POST /price` is not in the graded mix, so it costs ~0 CPU during the run. **This is a whole bonus category currently forfeited.**
2. **Check the Windows power plan** before trusting any further measurement (§8).
3. **Take the headline number on a cold machine** and say so explicitly in the write-up.
4. **Write the resilience write-up.** Required deliverable, doesn't exist, and the material is unusually strong — see §14.
5. **Correct the ARM strategy** and reconcile the earlier x86 audit.
6. **Delete `perf/x86-fused-x2-interleave`** — redundant with main.
7. *Optional:* asymmetric x3; `IO_THREADS=1` retest.

---

## 14. Material for the write-up

The track's on-theme prize rewards *resilience by design* over luck. What this session produced that reads as deliberate engineering:

- **Derived that score ∝ throughput** from the fixed client mix, then validated `score = 2.5 × requests` to **0.02%** on hardware it wasn't derived on.
- **Identified that load shedding is a trap** — a 1% error ceiling means you can shed 1 request in 100, so the lever is admission control, not rejection.
- **Discovered the measurement instrument had a 40% noise floor and built a 1% one** before trusting any optimisation decision. Then discovered the *cause* was thermal, and that only alternated A/B designs are valid on this hardware.
- **Found a 418-line back end that had never executed anywhere**, ran it, and measured it at +28.5%.
- **Then found it was structurally x1-with-memory-traffic and fused it: 2.389×**, `/risk` p95 496 → 205 ms, with the fast path still ~590× under its bar.
- **Explained where it stops and why, from the register file** — 6 vectors per lane, 16 XMM, `SHA256RNDS2`'s implicit XMM0 ⇒ 15 of 16 used; a third lane wants 21. Falsifiable, and it predicts the ARM/x86 difference correctly.
- **Killed a plausible-sounding optimisation with an hour of benchmarking** rather than building it — and can show the 60× measurement that proves the AVX–SSE transition penalty.
- **Kept a build-gated correctness ladder throughout**, because a wrong digest scores zero and looks exactly like a right one.

The last two are the strongest. Knowing what *not* to build, and proving it, is the part that separates engineering from luck.
