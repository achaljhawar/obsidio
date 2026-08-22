# Power-plan correction: verifying the 1.49 GHz finding

Follow-up to `x86-session-findings.md` §8 (thermal throttling). That section found the box
degrading ~2.18× over a session and attributed it to a clock drop from
~4 GHz nominal to **1.49 GHz measured**, then flagged the right question:

> 1.49 GHz is low even for a throttled laptop — check the Windows power plan
> and whether the machine is on battery. If the CPU is being capped by policy
> rather than heat, every measurement including the "cool" 5.8M is understated.

This document turns that open question into a procedure: audit the instrument,
audit the policy, distinguish heat from cap, and re-baseline if needed.

**Status: answered. See §5 for the measured verdict — it was neither heat
nor policy; the 1.49 GHz reading was an instrument artifact.**

---

## 0. Why this matters

| If 1.49 GHz is... | Consequence |
|---|---|
| **Heat** (genuine throttle) | Numbers are real but machine-specific; efficiency work (NEXT-LEVERS §2) directly buys clock back |
| **Policy** (power plan / battery / OEM cap) | Every number ever recorded on this box — including the "cool" 5.8M — is understated by up to 2.7×; one settings change re-rates the entire dataset |

Also note the internal tension worth resolving: findings reports score degrading
2.18× while clock allegedly degraded 2.68×. If both numbers are right, ~20% of
the workload did not scale with clock (memory-bound or fixed-latency paths) —
worth knowing which, because that part is immune to thermal fixes either way.

## 1. Audit the instrument FIRST — the 1.49 may be an artifact

**Nobody documented how the clock figure was produced**, and the two usual
in-container methods are both untrustworthy here:

1. `/proc/cpuinfo` MHz fields inside **WSL2 are virtualized and frequently
   static or nonsense**. Do not use.
2. **RDTSC/TSC timing is invariant by design** — the counter runs at a fixed
   frequency regardless of core throttle state. Any "clock" derived from TSC
   elapsed-time calibration measures nothing about sustained speed. Do not use.

### The correct in-container instrument

What scoring cares about is *effective compute throughput per second*, so
measure exactly that: run a dependent operation chain whose per-iteration cost
is known from microarchitecture tables, time N iterations with
`steady_clock`, and report effective GHz.

`starters/cpp/bench/effective_clock.cpp` (added on this branch) does this with
a serial FP-add chain (`addsd`: 3 cycles/link on Zen, 4 on Intel — pass the
right one, see §5.2), reporting:
```
effective-clock: <X.XX> GHz   (chain latency=3.0 cyc/link, window=2.0s, best-of-3)
```

Validate it once against a trusted Windows-side reading (HWiNFO64 / Ryzen
Master / Task Manager → Performance → CPU "Speed"), record the correction
factor if any, and from then on it is the canonical clock metric — usable
inside any container, WSL2 distro, cloud VM, or unknown grading hardware.

### Sanity expectations for this SKU-class

An 8C/16T Ryzen sustaining all-core load should hold roughly 70–90% of boost
on adequate cooling. **1.49 GHz is ~35% of the ~4.2 GHz this box actually
sustains all-core (§5.2) — far below genuine thermal behaviour for this
class** unless cooling has failed outright, which is why a policy cap was the
leading suspect going in. It turned out to be neither.

## 2. Windows-side checks (run these on the box, in order)

```bat
:: 1. Active plan
powercfg /getactivescheme

:: 2. Processor min/max state under the active plan
powercfg /q SCHEME_CURRENT SUB_PROCESSOR

:: 3. Battery vs AC — confirm it is plugged in
powercfg /batteryreport
```

Then visually confirm while a graded-style load runs:

- **Task Manager → Performance → CPU**: current clock + whether "Power saver"
  style behaviour shows.
- **HWiNFO64** (best evidence): watch per-core clocks AND the throttling flags
  row — it distinguishes **"Thermal Throttling: Yes"** from **"PPT/Power
  Limit: Yes"**. That single flag pair answers the heat-vs-policy question
  definitively.

If the machine is a laptop: plug in before anything else. On battery, Windows
caps aggressively regardless of plan.

If the plan is Balanced/Power-saver or max processor state < 100%: switch to
High performance (or set minimum/maximum processor state to 100%), then
re-run the cooldown experiment from `x86-session-findings.md` §8 unchanged. If the sag
disappears → policy; done, re-baseline everything. If it persists with HWiNFO
showing thermal-flag trips → genuinely thermal; proceed with the efficiency
lever instead.

One more WSL2-specific check: Windows 11 core parking / VBS (memory
integrity) can distort VM scheduling. If HWiNFO shows healthy clocks but
WSL2-measured effective-clock stays low, the gap is hypervisor overhead, not
CPU frequency — record it as such rather than as throttling.

## 3. Interpretation table

| Observation after fixes | Verdict | Action |
|---|---|---|
| Clock holds ≥ ~80% of nominal through a full graded run | Policy cap was the cause | Re-run headline numbers; update x86-session-findings.md §8 with corrected figures |
| Still sags; HWiNFO shows Thermal flag | Genuine thermal | Keep numbers as-is; pursue NEXT-LEVERS §1–§2 (waste = heat = lost clock) |
| Still sags; HWiNFO shows PPT/Power-limit flag | OEM firmware power budget | Treat like thermal for scoring purposes; note it — graders' box may differ |
| Windows-side clock fine, in-container effective-clock low | Hypervisor/scheduling overhead | Document as WSL2 tax; do not call it throttling |

## 4. What to re-baseline afterwards

Whichever way it resolves, redo once on corrected footing:

1. Cold-machine graded run (headline number for the write-up).
2. Back-to-back decay curve (clock at t=0 vs t=270s alongside work_score).
3. The alternating A/B probe pairs for any optimisation claims.

And update `x86-session-findings.md` §8's caveat with the verdict — that section already
promises the reader the answer is pending.


---

## 5. Results — measured 2026-08-22

The procedure above was run. **Verdict: neither heat nor policy. The 1.49 GHz
figure was an instrument artifact.** The clock never collapsed.

### 5.1 Policy audit (§2) — clean

```
powercfg /getactivescheme
  Power Scheme GUID: 27fa6203-...-748559d549ec  (performance)

powercfg /q SCHEME_CURRENT SUB_PROCESSOR
  PROCTHROTTLEMAX   AC: 0x64 (100%)   DC: 0x64 (100%)
  PROCTHROTTLEMIN   AC: 0x50  (80%)   DC: 0x05   (5%)

Win32_Battery.BatteryStatus = 2  -> on AC
Win32_Processor.MaxClockSpeed  = 3201 MHz
```

Maximum processor state is 100% on **both** AC and DC, and the machine was
plugged in. There is no policy cap. The leading suspect is eliminated.

Note the second correction hiding in that output: **nominal is 3.2 GHz base,
not the "~4 GHz" x86-session-findings.md §8 assumed.** The ~4 GHz figure was a boost
number used as if it were a baseline, which inflated the claimed ratio.

### 5.2 Sustained-load curve (§1 instrument) — a 1.7% sag, not 2.68×

`effective_clock` built into a container off the pinned build stage, then run
16-up (one container per core, `--cpuset-cpus=$i`) for 5.5 minutes of
all-core load. `latency=3` (Zen `addsd`); `latency=4` yields a physically
impossible 6.19 GHz, which confirms the 3-cycle figure for this part.

```
all-core mean effective-clock vs elapsed
    0- 30s   4.054 GHz
   60- 90s   3.991 GHz
  150-180s   4.042 GHz
  240-270s   3.999 GHz
  300-330s   3.984 GHz
                              n=2932 readings, min 3.171, mean 4.012
```

**4.054 -> 3.984 GHz: a 1.7% decay over the full window.** Single-core idle
reads 4.637 GHz.

### 5.3 Windows-side validation (the §1 calibration step)

Sampled concurrently with that load, via
`\Processor Information(*)\% Processor Performance` x 3201 MHz base:

```
t+0s    4,360 MHz  (136.2% of base)
t+45s   4,292 MHz
t+90s   4,238 MHz
t+180s  4,234 MHz
t+270s  4,250 MHz  (132.8%)
```

Windows says the box sustains **~4.23 GHz all-core, flat**. The in-container
probe read 4.012 GHz against that — **~6% low**, the expected loop-overhead
margin above pure `addsd` latency. The instrument is validated; record 0.94
as its correction factor.

Also confirmed in passing: `/proc/cpuinfo` inside the container reported a
static `cpu MHz: 3193.912` throughout — the virtualized-and-useless reading
§1 predicted.

### 5.4 Why 1.49 GHz was wrong

The number came from the v3 AVX2-mix harness's own clock loop, and **that
source was never committed** — `git log --all --diff-filter=A` over every
branch shows no such file ever existed in the repo. It cannot be re-run or
audited; it survives only as a figure quoted in prose.

x86-session-findings.md §7.2 already documents that same harness getting the clock wrong
twice: v1 "miscounted (a 3-cycle chain divided by 2)", and v2 was folded to a
closed form by GCC and reported "5,747,126 GHz". v3 fixed the folding with an
`asm volatile` barrier — but nothing in the record shows it fixed the
cycles-per-link divisor that broke v1. A 3-cycle chain scored as if it were
~8.5 cycles would report ~1.49 GHz on a core actually running at ~4.2.

An unreproducible reading from a harness with two documented clock bugs, in
direct contradiction to two independent validated instruments, is not
evidence of throttling.

### 5.5 What this leaves open

The score degradation itself was measured and is not in dispute: 5,781,894 ->
2,656,259 `work_score`, with `/price` p95 degrading in step. **What is now
unexplained is the cause** — it was not clock. Since `/price` never touches
the hash chain, the mechanism is something that slows everything uniformly:
Docker Desktop VM memory pressure, page-cache/balloon behaviour across a long
session, k6 co-location on the same 16 threads, or accumulated background
load. That is the question §8 should now be asking.

**One honest gap in this run:** the load was a scalar-FP dependency chain,
which draws less package power than SHA-NI plus full request serving. It
rules out a 2.68x collapse; it does not by itself prove the graded workload
holds 4.2 GHz. Closing that costs one run — `effective_clock -w` on a spare
core alongside the actual graded k6 run, which now also produces the
NEXT-LEVERS §2 clock-vs-time graph directly.

### 5.6 Consequences

- x86-session-findings.md line 19 ("**thermally throttles to 1.49 GHz**") and the §8
  clock row are **retracted**. §8's own open question is answered: not policy,
  not heat, instrument.
- NEXT-LEVERS §2 ("efficiency is thermal headroom") loses its premise. With a
  1.7% sag there is no meaningful thermal feedback loop to exploit; demote it
  from P1.
- NEXT-LEVERS §7/§8 "Windows power plan check — possibly re-rates every
  number" is **closed, negative**: the plan was never capping, so nothing is
  re-rated upward.
- The `x86-session-findings.md` §8 conclusion that survives untouched is the one that
  mattered most: absolute numbers from this box drift badly, and only
  tightly-alternated A/B ratios are trustworthy. The cause changed; the
  methodological discipline it justified was correct anyway.

*Related: next-levers.md §1 (fast-path CPU diet) and §2 (efficiency = thermal
headroom) are the two levers whose value changes most depending on this
verdict.*
