# Ryzen measurement kit

Tooling for tuning the `/risk` kernel on the confirmed x86 grading box. Nothing
here is built into the image; it drives the shipped image from outside.

Everything in this directory exists because of one finding: **on the Ryzen
laptop, the full grading script had a ~40% run-to-run spread, and the same
image scored 5.78M cool and 2.66M hot.** A 3% question cannot be answered with
a 40% instrument, and a measurement taken while the clock is sliding is not a
measurement. See `docs/history/x86-session-findings.md` section 8 and
`docs/history/x86-coarse-audit.md` section 10.

## Prerequisites

Run from a Linux shell with Docker (WSL2 is fine — the harness talks to the
container over a user-defined bridge network, so the Windows port proxy is out
of the path). Build the image first:

```
docker build -t obsidio-cpp starters/cpp
```

**On Windows with no WSL2 distro, use
[`run-harness-windows.sh`](run-harness-windows.sh) and skip the rest of this
section** — see below.

## Running this from Windows without WSL2

The development box has Docker Desktop but no WSL2 distro of its own (only the
internal `docker-desktop` one) and no real `python3` (only the Windows Store
alias stub). `ab.sh` needs both. From Git Bash:

```
./run-harness-windows.sh --reps 3 "sha-ni:" "fallback:RISK_BACKEND=reference"
./run-harness-windows.sh --script config_sweep.sh --reps 3
RISK_PCT=0 ./run-harness-windows.sh --probe mixed --reps 3 "cheaponly:IO_THREADS=2"
./run-harness-windows.sh --shell        # poke around inside
```

Arguments pass straight through to `ab.sh` (or to whatever `--script` names).
`RISK_PCT`, `K6_CPUSET`, `SUT_CPUS`, `CLOCK_FLOOR_PCT`, `IMAGE`, `REPS`,
`DURATION`, `VUS` and `COOLDOWN` are forwarded from the environment.

It runs the harness inside a `docker:cli` container with `bash` and `python3`
added, mounting `/var/run/docker.sock` so it drives sibling containers. Three
details are load-bearing, and each one was re-derived from nothing once
already:

- **The repo is mounted at the absolute path the host daemon resolves**, not at
  a tidy `/repo`. `ab.sh` launches k6 with `-v "$HERE:/scripts:ro"`, and that
  nested `-v` is interpreted by the *host* daemon — so `$HERE` has to be a
  string the host daemon can turn back into real files. Mounting at `/repo`
  makes the daemon create an empty volume and k6 runs an empty script
  directory, with no error anywhere. Docker Desktop does resolve the legacy
  `/c/Users/...` form, so the mount is
  `-v "C:/Users/.../obsidio:/c/Users/.../obsidio"`: same path in and out.
- **An LF copy is what executes.** `core.autocrlf` is true here, so the
  working-copy scripts have CRLF and bash rejects `set -uo pipefail\r`. The
  repo's `.gitattributes` now pins `*.sh` to `eol=lf`, which fixes it at
  checkout; the script regenerates an LF copy under `.lf/` anyway, for working
  copies that predate that.
- **`MSYS_NO_PATHCONV=1` on every `docker run` carrying a `-v`.** Git Bash
  rewrites anything path-shaped, turning `/var/run/docker.sock` into
  `C:/Program Files/Git/var/run/docker.sock`. The script sets it for its own
  invocations; you need it for any you type yourself.

The first run builds the `obsidio-harness` image once (`docker rmi
obsidio-harness` to refresh it).

### Running the grading script from Windows

The full grading script needs none of the above — no nested mount, no
`python3` — so run it directly from Git Bash, with `MSYS_NO_PATHCONV=1` for the
`-v`:

```
docker network create obsidio-final
docker run -d --name fsut --network obsidio-final --cpus=2 --memory=2g obsidio-cpp
MSYS_NO_PATHCONV=1 docker run --rm --network obsidio-final --cpuset-cpus=8-15 \
  -v "$(pwd -W)/k6:/scripts:ro" -e TARGET=http://fsut:8080 \
  grafana/k6:latest run /scripts/grading.js
docker rm -f fsut && docker network rm obsidio-final
```

`pwd -W` is what gives the daemon a Windows path; a bare `$PWD` from Git Bash
is an MSYS path the daemon cannot resolve.

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

### 3. What is SCHED_IDLE costing? — *answered, and the sweep is now inert*

**Nothing.** Measured over six arms × 3 reps: the largest effect in any
direction was 2.7% and it was negative, and `cpu%` sat at 196–199% in *every*
arm including `idle`. The hypothesis needed the workers to be starved, which
would have shown as idle-arm `cpu%` well below the 200% ceiling. Both granted
CPUs are already saturated as shipped, so there is no stranded capacity for
another scheduling class to reclaim.
See `docs/history/ryzen-ceiling-findings.md` §2 step 3.

`sched_sweep.sh` is kept as the record of how that was priced, but it **no
longer sweeps anything**: the `RISK_SCHED` knob it drives lived only on
`perf/ryzen-ceiling` and was not merged, so every arm now runs `SCHED_IDLE` and
reports six identical results. Recover the knob with
`git show 8a6e653:starters/cpp/src/risk_pool.cpp` if the question is ever
reopened.

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
