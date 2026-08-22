# Ryzen measurement kit

Tooling for tuning the `/risk` kernel on the confirmed x86 grading box. Nothing
here is built into the image; it drives the shipped image from outside.

Everything in this directory exists because of one finding: **on the Ryzen
laptop, the full grading script had a ~40% run-to-run spread, and the same
image scored 5.78M cool and 2.66M hot.** A 3% question cannot be answered with
a 40% instrument, and a measurement taken while the clock is sliding is not a
measurement. See `docs/history/x86-session-findings.md` section 8 and
`obsidio-findings.md` section 10.

## Prerequisites

Run from a Linux shell with Docker (WSL2 is fine — the harness talks to the
container over a user-defined bridge network, so the Windows port proxy is out
of the path). Build the image first:

```
docker build -t obsidio-cpp starters/cpp
```

`K6_CPUSET` defaults to `8-15`; set it to CPUs that are **not** where the
2-core container lands. On an 8-thread machine use `K6_CPUSET=4-7`.

## The three questions, in order

### 1. Is the machine in a state where measuring means anything?

```
./ab.sh --reps 3 "sha-ni:" "fallback:RISK_BACKEND=reference"
```

You are not primarily reading the ratio here — you already know roughly what it
is. You are reading the last three columns:

- `clock=` — an effective-clock proxy relative to the first sample of the run.
  Anything under `CLOCK_FLOOR_PCT` (default 85%) prints a throttle warning. It
  is timed integer work rather than `/proc/cpuinfo`, which under WSL2 reports a
  static nominal figure and never sees the throttle at all.
- `temp=` — real sensor reading when the kernel exposes one. Usually `n/a`
  under WSL2; read it on the Windows side instead (HWiNFO, or
  `Get-Counter '\Thermal Zone Information(*)\Temperature'`).
- `spread%` in the summary — if this is not around 1%, stop and fix the
  environment before trusting any comparison below it.

Raise `--cooldown` (default 20s) until consecutive reps of the same arm agree.

### 2. Which thread configuration wins here?

```
./config_sweep.sh --reps 3
```

Sweeps the committed `IO_THREADS=2 RISK_WORKERS=2` against `1/2` and `1/3`,
leaving `RISK_QUEUE` and `RISK_DEADLINE_MS` at their defaults.

This uses the **mixed** probe, and that is not a detail. `/risk` is handed to
the worker pool and never runs on an IO thread, so under a risk-only load the
epoll loops sit idle and every `IO_THREADS` value measures identically. The ARM
result being re-tested (+3.6–3.9%) was a full-mix result.

### 3. What is SCHED_IDLE costing?

```
./sched_sweep.sh --reps 3
```

Sweeps the worker scheduling class. Read `score` and `price_p95` together --
the winner is the highest score that still leaves comfortable latency margin,
not the highest score. See the header of `sched_sweep.sh` for why this uses the
mixed probe and why the trade is worth re-pricing.

### 4. Does the tuned kernel pay on this silicon?

```
./ab.sh --reps 3 "baseline:RISK_X86_KERNEL=baseline" "hoisted:RISK_X86_KERNEL=hoisted"
```

Both variants are in the same binary, so this A/B has no build-to-build
variance in it. Check the `serves:` line: the two arms must report **different**
back-end names, otherwise a typo silently compared the default against itself.

## Reading the verdict

The summary applies the agreed gate: a change is a `WIN` only at **≥2%** with
**non-overlapping ranges** across reps. Overlapping ranges print
`INCONCLUSIVE` no matter how good the means look, because with n=3 a mean
difference inside the noise band is not evidence.

`max fail%` must stay at 0. The grading gate is 1% `http_req_failed` and the
target is ≤0.2%; a kernel change that buys throughput by dropping requests is
not a win, it is a scored loss.

## Options

| Flag | Default | Notes |
|---|---|---|
| `--reps N` | 3 | Fewer than 2 and spread is unmeasured; the summary says so |
| `--duration` | `40s` | Per arm, per rep |
| `--probe` | `risk` | `risk` for kernel work, `mixed` for anything touching IO threads |
| `--cooldown` | 20 | Seconds between arms — the main thermal control |
| `--vus` | 16 / 32 | Risk / mixed probe defaults |
| `--image` | `obsidio-cpp` | |

Environment: `K6_CPUSET`, `SUT_CPUS`, `CLOCK_FLOOR_PCT`, `IMAGE`.

## After the kernel question is settled

Confirm with the real grading script before believing anything, because the
probe deliberately does not test the ramp, the 200-VU connection churn, or the
latency thresholds:

```
docker run --rm --network obsidio-ab-net --cpuset-cpus=8-15 \
  -v "$PWD/k6:/scripts" -e TARGET=http://obsidio-ab-sut:8080 \
  grafana/k6:latest run /scripts/grading.js
```

Expect that number to move a lot between runs. It is for threshold
confirmation and a headline figure, not for deciding a few percent.
