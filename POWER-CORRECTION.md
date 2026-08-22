# Power-plan correction: verifying the 1.49 GHz finding

Follow-up to `findings.md` §8 (thermal throttling). That section found the box
degrading ~2.18× over a session and attributed it to a clock drop from
~4 GHz nominal to **1.49 GHz measured**, then flagged the right question:

> 1.49 GHz is low even for a throttled laptop — check the Windows power plan
> and whether the machine is on battery. If the CPU is being capped by policy
> rather than heat, every measurement including the "cool" 5.8M is understated.

This document turns that open question into a procedure: audit the instrument,
audit the policy, distinguish heat from cap, and re-baseline if needed.

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
a serial FP-add chain (4-cycle latency per link on all modern x86), reporting:

```
effective-clock: <X.XX> GHz   (dependent add-chain, 4 cycles/link)
```

Validate it once against a trusted Windows-side reading (HWiNFO64 / Ryzen
Master / Task Manager → Performance → CPU "Speed"), record the correction
factor if any, and from then on it is the canonical clock metric — usable
inside any container, WSL2 distro, cloud VM, or unknown grading hardware.

### Sanity expectations for this SKU-class

An 8C/16T Ryzen sustaining all-core load should hold roughly 70–90% of boost
on adequate cooling. **1.49 GHz ≈ 37% of a ~4 GHz nominal is far below genuine
thermal behaviour for this class** unless cooling has failed outright — which
is precisely why a policy cap is the leading suspect.

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
re-run the cooldown experiment from `findings.md` §8 unchanged. If the sag
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
| Clock holds ≥ ~80% of nominal through a full graded run | Policy cap was the cause | Re-run headline numbers; update findings.md §8 with corrected figures |
| Still sags; HWiNFO shows Thermal flag | Genuine thermal | Keep numbers as-is; pursue NEXT-LEVERS §1–§2 (waste = heat = lost clock) |
| Still sags; HWiNFO shows PPT/Power-limit flag | OEM firmware power budget | Treat like thermal for scoring purposes; note it — graders' box may differ |
| Windows-side clock fine, in-container effective-clock low | Hypervisor/scheduling overhead | Document as WSL2 tax; do not call it throttling |

## 4. What to re-baseline afterwards

Whichever way it resolves, redo once on corrected footing:

1. Cold-machine graded run (headline number for the write-up).
2. Back-to-back decay curve (clock at t=0 vs t=270s alongside work_score).
3. The alternating A/B probe pairs for any optimisation claims.

And update `findings.md` §8's caveat with the verdict — that section already
promises the reader the answer is pending.

*Related: NEXT-LEVERS.md §1 (fast-path CPU diet) and §2 (efficiency = thermal
headroom) are the two levers whose value changes most depending on this
verdict.*
