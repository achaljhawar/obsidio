# Obsidio: the next levers

New ideas beyond everything in `arm64-strategy-notes.md`, `x86-session-findings.md`, and
`X86-THEORETICAL-LIMIT.md` (a working note that was never committed to this
repository). Written after reconciling all three against the
measured reality on the amd64 box (Ryzen 7 170, WSL2, fused x2 kernel,
~5.8M cold / ~2.7M hot, realistic ceiling ~6.4M via asymmetric x3).

Honesty clause up front: **none of these is another 2×.** Together they are
worth maybe +5–10% of score — but two of them are levers on *how this track is
judged* rather than on throughput, which is where an actually-unbeatable
advantage can still be built.

---

## 1. The fast-path CPU tax — a hidden ~8% leak (audit before building)

`x86-session-findings.md` §9 contains a number nobody has exploited:

> Chain free (fast path only: **79,080 req/s at 153% CPU**)

That is ~**19 µs of CPU per request** spent on everything that is not hashing.
At the graded mix (~8,600 req/s average, 90% of it cheap endpoints), roughly
**0.15–0.2 cores of the 2-core budget goes to syscalls, parsing, epoll
wakeups, and JSON formatting instead of SHA rounds** — call it 8–10% of total
capacity.

Nobody knows where those microseconds go yet. Candidates, in order of usual
guilt:

1. `snprintf("%g")`-style double formatting in `render_price`/`render_stats`
   — notoriously ~1 µs per call, several calls per response. Integer or
   fixed-point formatting is legal: the grader verifies only the `/risk`
   digest; `/price` and `/stats` need correct shape, not a specific float
   renderer (and caching stats answers stays forbidden — the *computation*
   must happen per request; only its cost is ours to cut).
2. Syscall crossings — one `recv` + one `send` per request at minimum, plus
   `epoll_wait` wakeups and the completion-path `eventfd` write per risk job.
   Edge-triggered epoll or io_uring batching could shave crossings, if WSL2's
   kernel allows io_uring through Docker Desktop (test first; many runtimes
   disable it).
3. Per-request allocations on the response path (`std::string` growth).

First step is a 30-minute profiling pass inside the container (`perf` if
available, otherwise targeted microbenchmarks). Caveat: verify the 19 µs is
not partly k6 co-location artifact from the probe that produced it.

Expected: +3–8%. Effort: medium.

## 2. Efficiency is thermal headroom — the compounding lever

The unique physics angle this hardware hands us. The box throttles to
1.49 GHz under sustained load (`x86-session-findings.md` §8). Every joule burned on
non-hash overhead is a joule that lowers the sustained clock during the
peak-hold minute — where most points are scored.

So cutting waste does not merely reclaim cycles directly; it slows heat
accumulation and keeps clocks higher *later in the run*. Waste less → stay
cooler → clock higher → serve more per watt. A positive feedback loop, and a
graph no other team will have: **core clock at t=0 vs t=270s, before and
after the §1 fixes.**

Expected: +1–3% directly, plus prize equity (see §6). Effort: low.

## 3. Spin-then-sleep workers (micro, ramp-edge)

Workers sleep in `cv_.wait()` when the queue empties; wakeup latency adds
bubbles exactly when batches are starving anyway — the ramp phases, which are
a third of the run and also the *coolest, fastest-clock* minutes. A short
spin (tens of µs) before sleeping keeps the SHA unit fed through transitions.
SCHED_IDLE permits spinning since any normal-priority thread preempts it
instantly. Measure with the alternating probe only.

Expected: +0–1%. Effort: low.

## 4. vzeroupper hygiene audit (trivial insurance)

`x86-session-findings.md` §7 proved legacy-SSE/VEX switching costs 60×. The fused kernel
is pure SSE+SHA-NI — clean — but round zero goes through OpenSSL, whose
optimized transforms may execute AVX2 assembly and leave dirty upper YMM
state that stalls the first SHA-NI rounds of the chain. One `vzeroupper`
after round zero (or forcing OpenSSL's SSE path for that single call) closes
it. Five minutes.

Expected: +0–0.5%. Effort: trivial.

## 5. Asymmetric x3 — the one live kernel idea (already flagged, here is the shape)

From `x86-session-findings.md` §9: block 2's message schedule is a compile-time constant,
so a third lane needs **no schedule registers there** — it spills only during
block 1. Concretely: fuse lanes as (pair + single), share the block-2
constants across all three, accept the block-1 spill window, and measure
whether hiding rnds2 latency for three chains beats the spill traffic. This
is the remaining ~12% gap toward the ~6.4M ceiling; findings estimates half
of it is claimable. Medium-high effort, uncertain payoff, alternating-probe
measurement mandatory.

## 6. Win the other prize simultaneously — the actual winning move

This track awards two things: the leaderboard score and the on-theme prize for
*"resilience by design."* Competition will show latency graphs. This project
has something almost nobody else will:

- A falsified optimisation with a controlled experiment — the AVX2 hybrid,
  killed by a measured **60×** AVX–SSE transition penalty before anyone wrote
  it (`x86-session-findings.md` §7).
- A lane count **derived from the register file before coding** (15/16 XMM at
  x2, 21 wanted at x3) that correctly predicts the ARM/x86 difference.
- A measurement-instrument story: discovered the graded script had a 40%
  drift floor on real hardware, built a ~1% alternated-A/B probe, then traced
  the drift to thermal throttling and quantified it.
- A build-gated correctness ladder, because a wrong digest scores zero and
  looks exactly like a right one.

`x86-session-findings.md` §14 is a prize-winning script already drafted. Hours spent
finishing that write-up likely earn more rank than hours chasing the last 3%
of throughput — and unlike the score race, almost nobody else is running it.

## 7. Still open from earlier rounds (unchanged by any of the above)

| Item | Status | Expected |
|---|---|---|
| Persistence bonus | **forfeited as shipped** — no `/data`, no `VOLUME`; compose still Node+Postgres | whole bonus category |
| Resilience write-up | **required deliverable, does not exist** | disqualification risk |
| Windows power plan check | if policy-capped not heat-capped, every number including "cold" is understated | unknown, possibly large |
| `IO_THREADS=1` retest on x86 | never run; needs mixed-workload probe | +3–4% candidate |
| SMT affinity | 8C/16T box; workers on sibling hyperthreads share one SHA unit and halve throughput; check WSL2 topology passthrough | up to +100% if unlucky, 0% if lucky |
| LTO / PGO / clang-vs-gcc | untouched Release flags | +1–3% |
| Batch-collect wait in pool | ramp phases starve batches; wait-to-fill keeps x2 path | +1–3% |
| Pre-warm before bind | cold-start artifacts land in scored warm-up minute | small, trivial |
| Purge tracked build artifacts (~110 files); fix selftest count (56→63); correct `arm64-strategy-notes.md` arm64 claims; reconcile `x86-coarse-audit.md`; delete redundant branch | repo hygiene | credibility |

## 8. The ledger

| Lever | Expected | Effort | Priority |
|---|---|---|---|
| Persistence bonus | bonus category | ~20 min | **P0** |
| Resilience write-up | required + prize | hours | **P0** |
| Power plan check | possibly re-rates every number | minutes | **P0** |
| Fast-path CPU diet (§1) | +3–8% | medium | P1 |
| Thermal story (§2) | +1–3% + prize equity | low | P1 |
| IO_THREADS=1 probe | +3–4%? | low | P1 |
| SMT affinity | insurance vs −50% | low | P1 |
| Batch-wait, pre-warm, LTO/PGO | +2–5% combined | low-med | P2 |
| Asymmetric x3 (§5) | up to +6% | med-high | P3 |
| Spin-then-sleep, vzeroupper | +0–1.5% | trivial | opportunistic |

Framing to keep in mind: the grading box is identical for every team, so the
absolute clock never decides the winner — **ratio-to-ceiling does**. On the
evidence, this codebase sits closer to its ceiling than any plausible
competitor. The residual risks are execution risks: forfeited bonuses,
missing deliverables, uncorrected documents. Close those first, then spend
remaining hours on §1/§2 and §6.

*Ceiling context: ~5.8M measured cold, ~6.4M realistic with asymmetric x3,
both ± sustained clock. Everything above pushes toward the second number or
protects the first.*
