# How to test `perf/ryzen-ceiling` on the Ryzen

You are running this on an **AMD Ryzen 7 170, Windows 11, Docker Desktop / WSL2**.
Nothing here changes how the server behaves by default — every new knob defaults
to current behaviour. This branch exists to answer three questions with numbers.

**Run everything from a WSL2 bash shell.** Not PowerShell, not Git Bash. Git
Bash mangles `-v` container paths and its `python3` hits the Windows Store alias
stub; both have already burned a session on this project.

---

## The three questions

| # | Question | Decides |
|---|---|---|
| 1 | Is `SHA256RNDS2` latency bound or port bound on this CPU? | Whether the SHA kernel is finished, or has ~2× left in it |
| 2 | What is `SCHED_IDLE` on the hash workers actually costing? | Possibly ~20% of the score |
| 3 | What does the cheap path (`/price`, `/stats`) really cost in CPU? | How much of question 2's gap is recoverable at all |

Background for each is in the source comments: `bench/x86/rnds2_ports.cpp`,
`src/risk_pool.cpp`, and `bench/ryzen/sched_sweep.sh`.

---

## Step 0 — Environment (do not skip)

The prior session on this machine saw the same image score 5.78M cool and 2.66M
hot: a 2.68× clock collapse, not a code regression. Every number below is
worthless if the machine throttles mid-run.

- Plug in the laptop.
- Windows power mode → **Best Performance**.
- Close everything else, especially browsers and other containers.
- Start from a **cool** machine, and let it cool between the long sweeps.

```bash
git fetch origin && git checkout perf/ryzen-ceiling
docker build -t obsidio-cpp starters/cpp
docker build --target build -t obsidio-build starters/cpp
nproc                       # expect 16; K6_CPUSET below assumes 8-15 are free
python3 --version           # the harness needs it
```

Sanity check that nothing regressed — this must pass before anything else
matters:

```bash
docker run --rm obsidio-build sh -c 'cd build && ctest --output-on-failure'
```

Expect `100% tests passed, 2 tests`.

---

## Step 1 — The ISA measurement (~2 min, run this first)

This one is decisive and cheap. It runs single-threaded, needs no container
orchestration, and everything else in the kernel depends on its answer.

```bash
docker run --rm obsidio-build ./build/obsidio-rnds2-ports
```

**What to look for**, in order:

1. `lanes to saturate: N` — the headline. It is a ratio measured inside one
   run, so it is immune to clock drift.
   - **N ≈ 2** → the kernel is port bound. The shipped two-lane design is at the
     instruction's throughput limit. The SHA kernel is *finished* and we should
     never touch it again.
   - **N ≈ 4** → the kernel is latency bound and is leaving roughly 2× on the
     table. The register-pressure argument that capped us at two lanes was never
     the real constraint, and building the phase-split kernel becomes the single
     highest-value thing left.
2. The `vs 1 lane` column — should rise then flatten. Where it flattens *is* the
   answer; the printed verdict is just reading the same curve.
3. The **port contention** section at the bottom. If AVX2 work costs SHA-NI
   almost nothing, there are spare vector issue slots and the SHA-NI + AVX2
   hybrid (previously killed on the assumption of port saturation) reopens.

Run it **twice** and confirm `lanes to saturate` agrees. If the two runs
disagree, the machine is too noisy — cool it down and retry.

## Step 2 — What the shipped kernel actually achieves (~2 min)

```bash
docker run --rm obsidio-build ./build/obsidio-bench-chain
```

It verifies the digest against the known golden before timing anything, then
prints chains/s for x1, x2, x3, x4.

Read it **against step 1**:

- Step 1 says 2 lanes saturate, and `x2` here is ≈2× `x1` → consistent, kernel
  done, stop.
- Step 1 says 4 lanes saturate, but `x4` here is ≈ `x2` → the hardware can
  absorb more work than the kernel is feeding it. That gap is exactly what a
  redesign would recover, and it is the strongest possible argument for building
  it.

Also worth noting: if `x4` is *worse* than `x2`, the pool's widest batch path is
paying register pressure for nothing and should be rewired to two pairs.

---

## Step 3 — Price the latency insurance (~25 min)

The claim: the hash workers run at `SCHED_IDLE`, scheduler weight **3** against
a normal thread's **1024**. While any IO thread is runnable the workers get
~0.3% of the CPU — effectively none. That is consistent with the risk-only probe
reaching ~1097 chains/s while the graded mix sustains ~849 with the queue never
empty.

What that buys is `/price` p95 of 336 µs against a **200 ms** bar. 595× of
headroom, and possibly a fifth of the score.

```bash
cd starters/cpp/bench/ryzen
./sched_sweep.sh --reps 3
```

Sweeps `idle → nice19 → nice10 → nice5 → batch → nice0` through the mixed
probe (mandatory: `/risk` never runs on an IO thread, so a risk-only load leaves
the epoll loops idle and every class measures identically).

**Read `score` and `price_p95` together — not score alone.** The winner is the
highest score whose `price_p95` still leaves comfortable margin. For calibration:
5 ms is still a 40× margin; 20 ms is still 10×.

Before trusting any of it, check:

- the `serves:` lines confirm the same back end in every arm;
- `spread%` per arm is ~1%. If it is 5%+, raise `--cooldown 60` and rerun;
- no `CLOCK DOWN` warnings;
- `max fail%` is **0.000** in every arm. A class that buys throughput by
  dropping requests is a scored loss, not a win.

The `cpu%` column is new: it is mean container CPU across the probe, so ~200%
means both granted CPUs were busy. If a faster arm shows *higher* cpu%, the win
is real utilisation rather than measurement drift.

## Step 4 — What the cheap path costs (~10 min)

Step 3 tells you what is recoverable; this tells you what the ceiling on that
recovery is. `RISK_PCT=0` removes `/risk` entirely, leaving only `/price` and
`/stats`:

```bash
RISK_PCT=0 ./ab.sh --probe mixed --reps 3 "cheaponly:IO_THREADS=2" "cheaponly1:IO_THREADS=1"
```

From the output compute, for each arm:

```
CPU-seconds per cheap request = (cpu% / 100) / (requests per second)
```

`requests/sec` is `score ÷ duration ÷ 1.0` here — with `RISK_PCT=0` the weighted
score is `0.6×1 + 0.3×3 = 1.5` per request, so `requests/sec ≈ score / (40 × 1.5)`
for the default 40s duration.

- If a cheap request costs **~5 µs** of CPU, then serving 7,641 of them per
  second needs about 4% of the budget — meaning most of the 22.6% gap is
  scheduling waste and step 3 should recover it.
- If it costs **~50 µs**, the IO path genuinely needs that CPU, step 3 will
  recover much less, and the real target becomes per-request overhead
  (syscall batching, io_uring) rather than scheduling.

This distinction decides where the next block of work goes, so please capture
the raw numbers even if they look boring.

---

## Step 5 — Confirm nothing regressed (~10 min)

Only after the above. The full grading script, unmodified:

```bash
cd /path/to/repo
docker network create obsidio-final 2>/dev/null || true
docker run -d --name fsut --network obsidio-final --cpus=2 --memory=2g obsidio-cpp
sleep 3
docker run --rm --network obsidio-final --cpuset-cpus=8-15 \
  -v "$PWD/k6:/scripts:ro" -e TARGET=http://fsut:8080 \
  grafana/k6:latest run /scripts/grading.js
docker rm -f fsut
```

All four thresholds green and `http_req_failed` at 0.00%. Expect this number to
swing a lot between runs — it is for threshold confirmation and a headline
figure, never for deciding a few percent.

If step 3 produced a winner, repeat this with that setting
(`-e RISK_SCHED=<winner>`) and compare thresholds, especially `/price` p95 under
the *real* 200-VU ramp rather than the probe's constant load.

---

## What to send back

1. Full output of steps 1 and 2 (both runs of step 1).
2. The step 3 summary table, including the per-rep lines with `cpu%` and
   `price_p95`.
3. The step 4 numbers and your computed CPU-per-cheap-request.
4. Step 5 threshold block and `work_score`, for the default and for any winner.
5. Anything that failed, hung, or looked wrong — including harness bugs. The
   harness has broken on this machine before and that is worth more than a
   clean-looking number.

## Please do not

- Draw conclusions from a single rep, or from any run with a `CLOCK DOWN`
  warning or >2% per-arm spread.
- Change a default because a sweep arm won. Report the numbers; the decision is
  a separate conversation.
- Trust the full grading script for anything under ~10% — it measured a 40%
  spread on this machine.

## Known-unfixed

`bench/ryzen/ab.sh` is bash and assumes a Linux shell. It has been run
successfully from WSL2 and fails from Git Bash. If you must use Git Bash,
`MSYS_NO_PATHCONV=1` fixes the `-v` mount but `python3` will still need to
resolve to a real interpreter rather than the Windows Store stub.
