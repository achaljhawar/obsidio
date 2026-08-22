# Score levers: what actually moves `work_score`

Written after re-deriving the cost model from `k6/grading.js` and the C++
source rather than from prose. Supersedes the lever ranking in
`NEXT-LEVERS.md` §8 where the two disagree, and depends on
`POWER-CORRECTION.md` §5 — several conclusions here only become visible once
the 1.49 GHz clock figure is retracted.

Every absolute number below is a measurement of **one laptop** (Ryzen 7 170,
Docker Desktop / WSL2). The reasoning is portable; the numbers are not. §7
says which is which.

---

## 1. Latency is not a lever, and that is load-bearing

`k6/grading.js:41-46` makes latency **pass/fail thresholds**, not a scored
quantity:

| tier | bar | measured (cool) | margin |
|---|---|---|---|
| price | p95 < 200 ms | 336 µs | **595×** |
| stats | p95 < 500 ms | 338 µs | **1479×** |
| risk | p95 < 1500 ms | 205 ms | **7.3×** |

And the leaderboard metric is a weighted count (`grading.js:26,78`), with a
fixed 60/30/10 mix at weights 1/3/10:

```
0.6×1 + 0.3×3 + 0.1×10 = 2.5   ->   work_score = 2.5 × completed requests
```

That is where `findings.md` §9's constant comes from, and it means **score is
purely throughput**. Nothing else in the scoring function is reachable.

### Why the latency goal collapses into the throughput goal

Risk p95 of 205 ms is ~1.8 ms of compute behind ~200 ms of **queueing**.
`grading.js` is a closed loop with no think time, so at peak each VU spends
`0.1 × 200 ms = 20 ms` of every ~23 ms iteration waiting on `/risk` — ~87% of
its time. Roughly **170 of 200 VUs sit in the risk queue** at any instant,
against 4 in service (2 workers × 2 lanes). Queue wait = depth ÷ service rate:

```
170 / 1000 chains per second  ~=  170 ms      (observed p95: 205 ms)
```

So risk latency is a *consequence* of throughput, not an independent quantity.
**The only way to cut it is to raise throughput** — the same lever as score.
There is no separate latency work worth doing, and 7.3× of margin exists to be
*spent*, not protected.

## 2. Where the CPU actually goes

Per 100 requests (60 price / 30 stats / 10 risk), at 1.82 CPU-ms per chain:

| | CPU | share of CPU | share of weight |
|---|---|---|---|
| risk chain | 18.2 ms | **91%** | 40% |
| fast path (90 × ~19 µs) | 1.7 ms | **9%** | 60% |

Every 1% of total CPU saved is +1% score. That is the entire optimisation
function. Note the inversion: the 60% of weight that comes from `/price` and
`/stats` costs 9% of the CPU.

## 3. Lever 1 — the ~20% gap between the kernel and its own floor

Only visible once the clock is corrected. The two-lane theoretical floor:

```
50,000 rounds × 64 rnds2 × 4 cyc latency / 2 lanes = 6.4M cycles/chain

6.4M @ 4.2 GHz sustained = 1.52 ms   (2-core container likely boosts higher -> ~1.39 ms)
measured                 = 1.82 ms
```

**~20-30% above the floor.** `findings.md` §5.4 claims the measurement "lands
on the two-lane floor" — but that arithmetic implies ~3.5 GHz, which
contradicts §8's own 1.49 GHz. The contradiction was invisible while the clock
was wrong, and it resolves in the direction of *more* headroom, not less.

Worth **up to +18% score** (the chain is 91% of CPU). The gap is not lane
count — it is message-schedule ops (`sha256msg1`/`msg2`/`padd`) competing for
ports, the `pshufb` hex conversion, and loop overhead. Wants a profile of
`chain2_impl` (`starters/cpp/src/chain_x86.cpp:519`), not a redesign.

## 4. Lever 2 — settle whether x3/x4 can help at all

The whole "~6.4M via asymmetric x3" ceiling rests on one unmeasured quantity:
`sha256rnds2` **reciprocal throughput**.

| if recip-tput is... | then |
|---|---|
| 2 cycles | x2 already saturates the port (128 rnds2 in 256 cyc). **x3 and x4 gain exactly zero, forever.** `findings.md` §9's remaining 12% and `NEXT-LEVERS.md` §5 are both dead |
| 1 cycle | x4 is worth up to **2×**, and the register-file argument in §5.3 is the wrong constraint to be reasoning about |

`findings.md` §7.3 tried to measure this and cannot be used: its "4.39 cycles
each" was computed with the retracted 1.49 GHz and becomes a nonsensical
12.4 cyc at the true clock, and its own caveat admits the setup was
latency-bound at 3 chains.

A 30-minute microbenchmark — N independent chains, N = 1..8, cycles per
instruction against N — decides whether a P3 item is P0 or should be deleted.
**Highest information value per hour of anything on this list.**

> **Corollary:** every absolute "cycles each" figure in `findings.md` is off
> by ~2.8×. The 60× AVX/SSE ratio in §7.3 survives — it is a ratio — but the
> ceiling analysis built on those cycle counts does not.

## 5. Lever 3 — fast-path diet (9% of CPU, ~19 µs/request)

19 µs is 3-4× more than the visible work accounts for. Three concrete finds:

1. **Redundant `EPOLL_CTL_MOD` on every request.**
   `starters/cpp/src/http_server.cpp:240-244` re-arms `EPOLLIN`
   unconditionally after a successful flush — but the fd is *already* armed
   `EPOLLIN`; `Connection` (`:51-58`) tracks no armed mask. That is 1 of ~3
   syscalls per request, wasted. Track the mask, call `MOD` only on change.
2. **Hash lookup per epoll event.** The loop stores `ev.data.fd` and then
   looks up `std::unordered_map<int, unique_ptr<Connection>>` (`:66`). Store
   `ev.data.ptr` instead — removes a hash plus pointer chase per event.
3. **`render_stats` serial FP reductions** (`starters/cpp/src/data.cpp:82-102`)
   — two 500-element chains, ~3,000 cycles of pure latency, unvectorisable at
   `-O2` without `-ffast-math`. Real, but only ~1.4% of the fast path. Last.

Halving the fast path = **+4.5%**. Items 1-2 carry the leverage.

The recv loop (`http_server.cpp:425-437`) already breaks on a short read, so
there is no wasted `EAGAIN` syscall — that one is done.

## 6. Lever 4 — batch to the backend's real lane width

`chain_x86.cpp:547-560`: `chain4_impl` is two **sequential** `chain2_impl`
calls and `chain3_impl` is `chain2` + `chain1`. But `risk_pool.cpp:97,106`
takes up to 4 jobs and only calls `on_done_` after **all** of them finish:

- `live == 4` — identical throughput to two batches of 2, but jobs 0-1 are
  held an extra ~1.8 ms for nothing.
- `live == 3` — the third lane runs `chain1` at **half** the per-chain rate.
  Leaving it queued to pair with the next arrival is ~33% better on that batch.

**Do not fix this by hardcoding 2.** `chain_arm.cpp:262-291` is a genuine
four-lane interleave — all four lanes advanced per round in one loop — so an
unconditional cap would *halve ARM's ILP*. See §7.

Correct fix: add a lane-width field to `Backend`
(`starters/cpp/src/chain_backend.hpp:22`) — 2 for x86, 4 for ARM — and have
the pool batch to `backend->lanes`. Portable, and it is where a measured
answer from Lever 2 lands anyway.

Small (+0-2%, mostly during ramp) but free and provably not-worse.

> `risk_pool.cpp:88-95`'s comment ("three cost ~37% more time than one") is
> **stale** — it describes the pre-fusion coarse kernel and is now false.

## 7. Portability: which of these survive a different grading box

**The grading architecture is not actually known.** Two repo documents
contradict each other and neither cites a source:

- `STRATEGY.md:287` — "Update 2026-08-22: the grading box is an arm64 Mac."
- `findings.md:26` — "The architecture question, settled by hardware."

§2's evidence is *"The machine is an AMD Ryzen 7 170... Docker runs
`linux/amd64`"* — that is the **dev laptop**, not the grader. The conclusion
does not follow from the premise: it is the same class of error as the
1.49 GHz reading, an observation about this box promoted into a claim about a
different one. `track.md` and `directions.md` say **nothing** about
architecture; the only hardware statement is `directions.md:25`, "capped at
2 CPUs and 2 GB RAM."

`CMakeLists.txt:12` states the operative fact: *"the grader builds this image
on its own machine."* No `--platform` pin, `-march=native` correctly banned,
backend chosen at runtime by CPUID. **The shipped artifact already handles
both**, so portability is the delivery model, not a hypothetical.

| Lever | Portability | Why |
|---|---|---|
| Scoring model (§1) | **Fully agnostic** | From `grading.js` + closed-loop queueing, not silicon |
| epoll `MOD` elision | **Fully agnostic** | Syscall count; zero CPU dependence |
| `ev.data.ptr` | **Fully agnostic** | Data structure |
| `render_stats` unroll | **Fully agnostic** | Every OoO CPU has FP-add latency |
| Lane-scaling measurement (§4) | **Agnostic method, per-CPU answer** | Exactly the tool for an unknown box |
| Kernel-gap work (§3) | **x86-SHA-NI general** | Structural on any SHA-NI part; the *20%* is Ryzen-at-4.2 GHz |
| Optimal lane count | **Microarch-specific** | `sha256rnds2` latency × recip-tput differs Zen vs Intel vs ARM |
| Clock/thermal, 1.82 ms/chain, 19 µs | **This box only** | Laptop clock + Docker Desktop / WSL2 seccomp |

What shifts per target:

- **Intel with SHA-NI** — x86 reasoning holds, but Ice Lake / Alder Lake+ are
  not Zen; the 4-cycle latency in §3's floor formula is a Zen number and the
  lane count must be re-measured.
- **arm64** — `chain_arm.cpp` serves, x4 is already right, and §5.3's register
  argument (15/16 XMM, implicit XMM0) is x86-only and irrelevant. §3 does not
  transfer; §5 transfers whole.
- **Native Linux instead of WSL2** — syscalls get cheaper, so the fast path
  shrinks below 9% and the epoll win gets *smaller* (still positive, never
  negative) while the chain's share rises above 91%.
- **A slower box** — the one place latency stops being free. Risk p95 is
  queueing-dominated, so it scales inversely with throughput. The 7.3× margin
  absorbs a lot, but thresholds are placeholders "finalised on the grading
  hardware," so do not bank it.

## 8. What to drop

- **`NEXT-LEVERS.md` §2 "efficiency is thermal headroom"** — dead. A 1.7% sag
  (`POWER-CORRECTION.md` §5.2) leaves no feedback loop to exploit. Demote
  from P1.
- **`NEXT-LEVERS.md` §7/§8 "power plan check — possibly re-rates every
  number"** — closed, negative. Nothing is re-rated upward.
- **Load shedding** — the arithmetic says riding the 1% error ceiling is worth
  ~+5% (shed 0.9% of requests, all risk: frees ~9% of CPU, costs 4% of
  weight). Still do not ship it: a hair's breadth from a hard threshold, and
  `grading.js:74` notes the grader may verify digests. `findings.md` §9 closed
  it and the conclusion is right, though the stated reasoning is not.
- **More threads, caching, GPU** — correctly closed in §9, unaffected by any
  of this.

## 9. Ranked

| Lever | Expected | Confidence | Effort | Portable? |
|---|---|---|---|---|
| Measure `rnds2` recip-throughput | decides ±10% and kills-or-promotes x3/x4 | — | 30 min | method yes |
| Close kernel gap to x2 floor (§3) | up to +18% | medium | medium | x86 only |
| Fast path: epoll `MOD` + `data.ptr` (§5) | +2-4% | high | ~1 hr | **yes** |
| Backend-driven batch width (§6) | +0-2% | high | ~15 min | **yes** |
| `IO_THREADS=1` retest (`findings.md` §12.4) | +3-4%? | untested on x86 | low | partly |

**Order of operations:**

1. **Ask the organisers the architecture question.** Two repo docs disagree
   and one of them costs a rewrite.
2. **Ship §5 now** — fully portable, no downside on any target.
3. **Make batch width backend-driven** (§6) — fixes the x86 bug without
   breaking ARM.
4. **Hold §3 and the lane-count decision** until the architecture is known,
   then run the scaling microbenchmark on the real target.

## 10. Corrections to `findings.md` implied by this document

| Location | Status |
|---|---|
| `:19`, §8 clock row — "thermally throttles to 1.49 GHz" | **Retracted** (`POWER-CORRECTION.md` §5) |
| §7.3 absolute cycle counts | **Wrong by ~2.8×**; the 60× ratio survives |
| §5.4 "lands on the two-lane floor" | **Wrong** — implies 3.5 GHz; there is 20-30% of headroom |
| §2 "the architecture question, settled by hardware" | **Not settled** — evidence is about the dev box |
| §9 "~6.4M via asymmetric x3" | **Unproven** — rests on an unmeasured recip-throughput |
| `risk_pool.cpp:88-95` comment | **Stale** — describes the pre-fusion kernel |

*Nothing in §3-§6 has been implemented. This is analysis against the tree at
`f47c549`, verified by reading `grading.js`, `http_server.cpp`,
`risk_pool.cpp`, `chain_x86.cpp`, `chain_arm.cpp`, and `data.cpp`.*
