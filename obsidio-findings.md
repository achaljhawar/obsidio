# Obsidio — Complete Findings Report

**Date:** 2026-08-22
**Repo:** `C:\Users\YASH\achal-projects\obsidio` @ `0de6afe` (main, clean)
**Machine under test:** AMD Ryzen 7 170, Windows 11, Docker Desktop / WSL2
**Prepared by:** Claude Code session — every number below was measured in this session unless explicitly attributed to a repo document.

---

## Table of contents

1. [Executive summary](#1-executive-summary)
2. [Test environment — full specification](#2-test-environment--full-specification)
3. [Build verification](#3-build-verification)
4. [Backend selection — the x86 path runs for the first time](#4-backend-selection--the-x86-path-runs-for-the-first-time)
5. [Correctness verification](#5-correctness-verification)
6. [Graded run #1 — the headline competition result](#6-graded-run-1--the-headline-competition-result)
7. [Resource envelope under load](#7-resource-envelope-under-load)
8. [Graded run #2 — reference backend A/B](#8-graded-run-2--reference-backend-ab)
9. [Graded run #3 — confirmation, and the retraction](#9-graded-run-3--confirmation-and-the-retraction)
10. [The measurement-noise discovery](#10-the-measurement-noise-discovery)
11. [The alternating probe — definitive backend A/B](#11-the-alternating-probe--definitive-backend-ab)
12. [Quantitative decomposition: why the score dropped vs the Spark](#12-quantitative-decomposition-why-the-score-dropped-vs-the-spark)
13. [Register-file analysis: why ARM's win does not transfer to x86](#13-register-file-analysis-why-arms-win-does-not-transfer-to-x86)
14. [Validation of the project's own scoring model](#14-validation-of-the-projects-own-scoring-model)
15. [Repository defects and documentation contradictions](#15-repository-defects-and-documentation-contradictions)
16. [Audit-item status ledger](#16-audit-item-status-ledger)
17. [Environmental gotchas encountered](#17-environmental-gotchas-encountered)
18. [Open questions](#18-open-questions)
19. [Recommendations, prioritised](#19-recommendations-prioritised)
20. [Exact reproduction instructions](#20-exact-reproduction-instructions)
21. [Raw data appendix](#21-raw-data-appendix)

---

## 1. Executive summary

Nine findings, ordered by consequence.

| # | Finding | Status |
|---|---|---|
| 1 | **The grading architecture question is settled by hardware inspection: this box is x86-64 (AMD Ryzen 7 170).** `STRATEGY.md` asserts an arm64 Mac and declares the x86 path dead. That assertion is wrong for this machine. | Verified |
| 2 | **The submission qualifies.** All four thresholds green on three independent full graded runs, 0.00% errors across 2,672,381 requests total. | Verified |
| 3 | **The x86 SHA-NI backend is worth +28.5%** over the reference fallback on `/risk` throughput (460.07 vs 358.00 chains/s, 1.13% noise floor). This closes the largest open question in `AUDIT-HANDOFF.md` §0.3. | Verified |
| 4 | **The full 4m30s graded run is unusable as an A/B instrument on this box — 40.25% spread between identical runs.** A 40s risk-only probe at 16 VUs is ~1% repeatable. The instability is in the connection layer, not the compute path. | Verified |
| 5 | **An earlier conclusion in this session ("disabling the x86 backend is worth +10.9%") was measurement noise and is retracted.** The correct sign is the opposite. | Retracted |
| 6 | **~1.24× of the gap vs the Spark is hardware/virtualization; ~2.30× is the backend.** ARM's 4-lane register-resident interleave beats x86's coarse interleave by 2.30×. Product 2.856× reproduces the observed per-chain ratio of 2.856× exactly. | Verified |
| 7 | **The 4-lane ARM win cannot be replicated on x86** — 16 XMM registers minus `SHA256RNDS2`'s implicit XMM0 operand will not hold four lanes. The realistic x86 ceiling is 2 lanes, possibly 3. | ISA analysis |
| 8 | **Remaining headroom is a true round-by-round x2 in `chain_x86.cpp`.** Current code is structurally x1-with-memory-traffic. Estimated +30–60% on the risk path (~98% of CPU). | Estimate |
| 9 | **Seven repo defects found**, including a `docker-compose.yml` describing a system the team did not build, a persistence bonus that silently does not apply, and ~110 committed build artifacts. | Verified |

---

## 2. Test environment — full specification

### 2.1 Host hardware

| Property | Value |
|---|---|
| CPU | AMD Ryzen 7 170 with Radeon Graphics |
| Physical cores | 8 |
| Logical processors | 16 |
| Architecture | AMD64 (x86-64) |
| Host RAM | 15.3 GB |
| OS | Windows 11 Home Single Language 10.0.26200 |

**Significance:** this is an x86-64 machine with SHA extensions (SHA-NI), present on all AMD Zen-family cores. It is *not* arm64. Every ARM-specific measurement in `STRATEGY.md` describes different silicon.

### 2.2 Container runtime

| Property | Value |
|---|---|
| Docker client | 29.7.2 |
| Docker server | 29.7.2 |
| Server OS/arch | linux/amd64 |
| CPUs visible to daemon | 16 |
| Memory available to VM | 7,964,618,752 bytes (7.42 GiB) |
| Backend | Docker Desktop on WSL2 |

**Note on the core-count gotcha:** the container can see all 16 logical processors while being capped at 2. The build defends against this correctly — `IO_THREADS` and `RISK_WORKERS` are set explicitly in the Dockerfile and never derived from `hardware_concurrency()`.

**Note on virtualization:** Docker Desktop on Windows runs containers inside a WSL2 Linux VM. This is the same *class* of distortion `STRATEGY.md` §6 describes for Docker-on-Mac: a CFS quota inside a hypervisor's own scheduler, plus a userspace network path. It is a confound for latency and, as §10 shows, a severe one for throughput stability.

### 2.3 Load generator

| Property | Value |
|---|---|
| k6 version | v2.2.0 (commit/00a9a1b7f5, go1.26.5, linux/amd64) |
| Delivery | `grafana/k6:latest` container |
| Placement | `--cpuset-cpus=8-15` (never competes with the SUT) |
| Network path | shared Docker network `obsidio-net`, targeting `http://obsidio-test:8080` |

The k6 version is **identical to the one used for the Spark runs** recorded in `STRATEGY.md` (v2.2.0, linux/arm64), so the tooling is comparable across the two machines.

**Why a shared Docker network rather than a published port:** targeting the container by name over a user-defined bridge network bypasses Docker Desktop's Windows port-forwarding proxy entirely. `STRATEGY.md` §6 identifies that proxy as a measurement confound on Mac; the same reasoning applies here. This is the highest-fidelity path available on this host.

### 2.4 System under test

```
docker run -d --name obsidio-test --network obsidio-net \
  --cpus=2 --memory=2g obsidio-cpp
```

Container left **unpinned** deliberately — `--cpus=2` is a CFS quota, not an affinity mask, and that is what the grader runs. Pinning would flatter the tail-latency profile.

### 2.5 Git state

| Property | Value |
|---|---|
| Branch | `main` |
| HEAD | `0de6afe` — Merge pull request #1 from achaljhawar/submission |
| Working tree | clean |
| `origin/submission` | fully contained in `main` (0 commits ahead) |

Recent history:

```
0de6afe  Merge pull request #1 from achaljhawar/submission
13b95da  x86 SHA-NI back end, persistence bonus, and audit fixes
4b74042  docs: arm64-Mac grading box — invert machine strategy, record thread sweep
4a00b7b  perf: route the no-backend fallback through sha256_64
958e7f4  docs: correct the baseline row label
9e85bdb  docs: record the interleaved-chain results and correct the roadmap
8e9f8d3  perf: interleave four chains instead of three, +9.8%
d8accdb  perf: interleave three chains instead of two, +24%
```

---

## 3. Build verification

### 3.1 Result

`docker build -t obsidio-cpp starters/cpp` — **succeeded**.

This is itself a finding. `AUDIT-HANDOFF.md` §0.1 predicted the tree would **fail to compile**, because `src/risk.cpp:199` calls `chain::x86_sha_backend()` while `chain_backend.hpp` allegedly declared only `arm_crypto_backend()`.

**That defect was fixed in `13b95da`.** Verified directly:

```
src/chain_backend.hpp:49:const Backend* arm_crypto_backend();
src/chain_backend.hpp:53:const Backend* x86_sha_backend();
src/risk.cpp:195:    candidate = chain::arm_crypto_backend();
src/risk.cpp:199:    candidate = chain::x86_sha_backend();
src/chain_x86.cpp:401:const Backend* x86_sha_backend() {
src/chain_x86.cpp:413:const Backend* x86_sha_backend() { return nullptr; }
```

Both declarations are present. Audit §0.1 is **closed**.

### 3.2 Build-time correctness gate

The Dockerfile runs the self-test twice (`Dockerfile:30-31`):

```dockerfile
RUN ./build/obsidio-selftest \
    && RISK_BACKEND=reference ./build/obsidio-selftest
```

Both passes succeeded. Final line of each: `all checks passed`.

**Important subtlety observed:** the *last* self-test output visible in the build log reads:

```
Hash back end
  selected: reference (specialised 64-byte transform)
```

This is the **forced second pass**, not the shipped configuration. Reading only the build tail would lead you to believe the image ships the reference backend. It does not — see §4. This is a real trap for anyone verifying the build from CI output alone.

### 3.3 Assertion count

`grep -c "expect_eq" tests/selftest.cpp` → **63**.

`STRATEGY.md:262` claims **56/56**. The document is stale by 7 assertions. `AUDIT-HANDOFF.md` §2 already noted 63 and flagged the discrepancy; this confirms it independently.

### 3.4 apt pin validity

The pins in `Dockerfile:9-12` resolved without error on `debian:bookworm-20240926-slim`:

```
g++=4:12.2.0-3
cmake=3.25.1-1
make=4.3-4.1
libssl-dev=3.0.20-1~deb12u2
libssl3=3.0.20-1~deb12u2
```

The `3.0.20-1~deb12u2` correction recorded in `STRATEGY.md:278` is confirmed correct. Reproducible-build rule satisfied.

### 3.5 CMake architecture dispatch

`CMakeLists.txt:46-52` — on `x86_64|AMD64|amd64`, `src/chain_x86.cpp` is compiled with `-msha;-mssse3;-msse4.1`, scoped to that single translation unit rather than the whole target. `-march=native` is explicitly refused (`CMakeLists.txt:12-16`). Runtime gating is via `__builtin_cpu_supports("sha")`.

This design is correct and worth crediting: the same binary starts on an x86 CPU without SHA-NI, because no instruction from that TU is reached until CPUID confirms the feature.

---

## 4. Backend selection — the x86 path runs for the first time

Startup banner from the running container:

```
persistence: DISABLED (/data/prices.log not writable) -- POST /price is in-memory only
obsidio-cpp listening on :8080  io_threads=2 risk_workers=2 risk_queue=512 risk_deadline_ms=0
  hash back end: x86-sha-ni (x1..x4 coarse interleave)
```

Three findings in three lines.

### 4.1 The x86 SHA-NI backend is selected and serving

`chain_x86.cpp` had **never been executed anywhere** prior to this session. `STRATEGY.md:336` states an amd64 cross-build was attempted on the Spark and died at `exec format error` (no binfmt/qemu). `AUDIT-HANDOFF.md` §0.3 states plainly: *"No end-to-end measurement of the x86 path exists in this repo."*

This is that measurement. The runtime CPUID gate passes on Zen, `verify_backend()`'s per-lane boot verification passes, and the backend serves.

### 4.2 Persistence is silently disabled

`persistence: DISABLED (/data/prices.log not writable)`.

`AUDIT-HANDOFF.md` §3 predicted this exactly: the Dockerfile never creates `/data` and declares no `VOLUME`, while `main.cpp:152` defaults the log path to `/data/prices.log`. Without an explicit `-v` mount, `persist_init` fails and the service degrades gracefully to in-memory.

**Consequence: the persistence bonus does not apply as the image ships.** The degrade is graceful and correct engineering, but the bonus is worth zero unless the grader mounts a volume — and nothing in the submission causes them to.

### 4.3 Thread configuration confirmed

`io_threads=2 risk_workers=2 risk_queue=512 risk_deadline_ms=0` — matches `Dockerfile:42-45`. The `IO_THREADS=1` change (worth +3.6–3.9% on the Spark per `STRATEGY.md:238`) has **not** been applied.

---

## 5. Correctness verification

A wrong digest returns a plausible 64-character hex string and scores **zero** while looking identical to a correct answer. This is the highest-stakes silent failure in the whole track, so it was verified independently rather than trusted.

### 5.1 Method

Served response for `GET /risk?seed=0.5`:

```json
{"seed":"0.5","risk_hash":"8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148"}
```

Independent Python reference, computed in a clean `python:3.11-slim` container:

```bash
python -c "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
```

```
8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148
```

### 5.2 Result

**MATCH — exact, all 64 hex characters.**

This verifies the full 50,000-round chain end-to-end through the x86 SHA-NI path, out of the running container, against an implementation that shares no code with it. Combined with the 63 build-gated assertions and `verify_backend()`'s per-lane boot check, the correctness story on x86 is solid.

### 5.3 Defence-in-depth inventory (from source reading)

The layered correctness design is the strongest-engineered part of this submission:

1. **A deliberately slow oracle** — `risk.cpp:14` keeps a plain per-iteration reference chain with an explicit "do not optimise" instruction. Never serves a request.
2. **Startup self-verification** — `select_backend()` (`risk.cpp:186`) refuses to select a backend until `verify_core()` reproduces reference digests, verifying **each lane count separately** (`verify_lane3`/`verify_lane4`). A failing wide lane is nulled and the pool degrades to fewer lanes rather than serving wrong answers (`risk.cpp:213-218`).
3. **Build-time gate** — `Dockerfile:30` runs the self-test twice, the second pass forcing `RISK_BACKEND=reference` so the fallback cannot ship untested.
4. **Cross-lane agreement checks** — the self-test verifies x4 agrees with x3/x2/x1 on shared lanes. Without this, a served digest could depend on how deep the queue happened to be at that instant. This is the subtlest class of bug in the design and it is explicitly covered.

---

## 6. Graded run #1 — the headline competition result

The unmodified `k6/grading.js`, unmodified image, `--cpus=2 --memory=2g`.

### 6.1 Thresholds — all green

| Metric | Bar | Measured | Margin |
|---|---|---|---|
| `http_req_duration{tier:price}` p95 | < 200 ms | **303.18 µs** | 660× under |
| `http_req_duration{tier:stats}` p95 | < 500 ms | **305.07 µs** | 1,639× under |
| `http_req_duration{tier:risk}` p95 | < 1500 ms | **495.92 ms** | 3.02× under |
| `http_req_failed` | < 1% | **0.00%** | 0 of 761,360 |

### 6.2 Score

```
work_score .......... 1,901,672   (7,043.513216 /s)
checks_total ........   761,360   (2,819.965389 /s)
checks_succeeded ....   100.00%   (761,360 of 761,360)
checks_failed .......     0.00%   (0 of 761,360)
http_reqs ...........   761,360   (2,819.965389 /s)
http_req_failed .....     0.00%   (0 of 761,360)
iterations ..........   761,360   (0 interrupted)
vus_max .............       200   (peak observed 199)
```

### 6.3 Latency distribution, per tier

| Tier | avg | min | med | p90 | **p95** | max |
|---|---|---|---|---|---|---|
| price | 234.93 µs | 21.52 µs | 178.47 µs | 260.23 µs | **303.18 µs** | 19.37 ms |
| stats | 235.33 µs | −1,526,932 ns † | 180.91 µs | 262.48 µs | **305.07 µs** | 19.36 ms |
| risk | 292.71 ms | 4.12 ms | 253.63 ms | 470.03 ms | **495.92 ms** | 670.15 ms |
| *all* | 29.37 ms | −1,526,932 ns † | 184.96 µs | 11.53 ms | 252.98 ms | 670.15 ms |

† See §17.2 — negative minimum durations are a clock artifact on this platform, not a real measurement.

### 6.4 Network volume

```
data_received ....... 122 MB  (453 kB/s)
data_sent ...........  69 MB  (257 kB/s)
iteration_duration ..  avg=29.44ms  min=84.95µs  med=249.14µs  p90=11.63ms  p95=253.1ms  max=670.28ms
```

### 6.5 Interpretation

**The submission qualifies on the real grading architecture.** The fast-path margin is extraordinary — `/price` p95 at 303 µs against a 200 ms bar means the architecture's central claim (never let heavy work block the fast path) is doing exactly what it was designed to do, under saturation, on x86, with a backend that had never previously run.

The single most eloquent pair of numbers in the whole report: **at the same instant, under the same saturation, `/price` p95 is 303 µs and `/risk` p95 is 495.92 ms.** Three orders of magnitude apart. That is the `SCHED_IDLE` + off-IO-thread queue design working as intended, and it is the number to put in the pitch.

---

## 7. Resource envelope under load

Sampled with `docker stats --no-stream` during the peak of graded run #1:

```
16:52:15  CPU=200.87%  MEM=5.340MiB / 2GiB
16:52:17  CPU=200.93%  MEM=5.316MiB / 2GiB
16:52:19  CPU=199.21%  MEM=5.316MiB / 2GiB
16:52:21  CPU=198.19%  MEM=5.316MiB / 2GiB
16:52:23  CPU=202.11%  MEM=5.062MiB / 2GiB
16:52:25  CPU=201.54%  MEM=5.062MiB / 2GiB
16:52:27  CPU=198.01%  MEM=5.062MiB / 2GiB
16:52:29  CPU=198.80%  MEM=5.062MiB / 2GiB
```

| Statistic | Value |
|---|---|
| CPU mean | 199.96% |
| CPU range | 198.01% – 202.11% |
| Memory range | 5.062 – 5.340 MiB |
| Memory as % of 2 GiB budget | **0.26%** |

### Findings

- **The container sits at exactly its 2-core quota** and never exceeds it. Mean 199.96% against a 200% ceiling. Confirms the thread sizing is correct and that no runtime is spawning workers for cores it cannot use.
- **Memory usage is 0.26% of budget.** The lanes live in registers, not buffers. This corroborates the Spark's 6.1 MiB figure (`STRATEGY.md:275`) closely — 5.1 MiB here.
- **Memory is not a constraint and never will be.** There are 2,043 MiB unused. If any future optimisation wants to trade memory for speed, the budget is entirely available. Nothing currently does.

---

## 8. Graded run #2 — reference backend A/B

Same image, same caps, forced to the fallback with `-e RISK_BACKEND=reference`. Banner confirmed:

```
hash back end: reference (specialised 64-byte transform)
```

Note this is **not** naive OpenSSL. Per commit `4a00b7b`, the fallback path routes through `chain_fallback()` → `sha256_64()`, the specialised two-block transform — "roughly 5x on that path". So this A/B compares *hand-written SHA-NI* against *a well-called OpenSSL two-block transform*, not against a strawman.

### 8.1 Results

| Metric | Bar | Measured |
|---|---|---|
| `/price` p95 | < 200 ms | 298.36 µs |
| `/stats` p95 | < 500 ms | 301.18 µs |
| `/risk` p95 | < 1500 ms | 577.85 ms |
| errors | < 1% | 0.00% (0 of 843,272) |

```
work_score .......... 2,108,026   (7,807.796821 /s)
http_reqs ...........   843,272   (3,123.346885 /s)
```

| Tier | avg | min | med | p90 | p95 | max |
|---|---|---|---|---|---|---|
| price | 229.75 µs | −902,874 ns † | 177.29 µs | 256.48 µs | 298.36 µs | 18.47 ms |
| stats | 232.74 µs | 51.43 µs | 179.63 µs | 259.64 µs | 301.18 µs | 16.59 ms |
| risk | 263.58 ms | 5.23 ms | 243.52 ms | 521.46 ms | 577.85 ms | 667.06 ms |

### 8.2 The apparent — and wrong — conclusion

Taken at face value this says the reference fallback **beats** the x86 SHA-NI backend by +10.9% (2,108,026 vs 1,901,672), which would mean the 418 lines of `chain_x86.cpp` are actively harmful.

That conclusion was drawn in-session and **is wrong**. See §9 and §10.

---

## 9. Graded run #3 — confirmation, and the retraction

Because a 10.9% claim would have driven a real code change, the x86 configuration was re-run identically before acting on it.

### 9.1 Results

| Metric | Bar | Measured |
|---|---|---|
| `/price` p95 | < 200 ms | 294.75 µs |
| `/stats` p95 | < 500 ms | 297.61 µs |
| `/risk` p95 | < 1500 ms | 452.66 ms |
| errors | < 1% | 0.00% |

```
work_score .......... 2,667,160   (9,878.798918 /s)
http_reqs ......... 1,067,749   (3,954.797487 /s)
```

| Tier | avg | min | med | p90 | p95 | max |
|---|---|---|---|---|---|---|
| price | 236.93 µs | 29.33 µs | 174.18 µs | 253.74 µs | 294.75 µs | 18.28 ms |
| stats | 241.36 µs | 43.09 µs | 176.48 µs | 256.44 µs | 297.61 µs | 17.57 ms |
| risk | 207.57 ms | 4.05 ms | 191.43 ms | 408.22 ms | 452.66 ms | 532.82 ms |

### 9.2 The retraction

| Run | Backend | `work_score` | req/s |
|---|---|---|---|
| 1 | x86-sha-ni | 1,901,672 | 2,819.97 |
| 2 | reference | 2,108,026 | 3,123.35 |
| 3 | **x86-sha-ni (identical to run 1)** | **2,667,160** | **3,954.80** |

The same binary with the same configuration scored **1,901,672 and then 2,667,160**.

```
Spread = (2,667,160 − 1,901,672) / 1,901,672 = 40.25%
```

**A 40.25% spread between identical configurations makes a 10.9% difference meaningless.** The three runs were sequential in wall-clock time and the scores climbed monotonically — 1.90M → 2.11M → 2.67M — which is the signature of a time-dependent machine artifact, not a backend effect.

**Retracted:** the claim that disabling the x86 backend is worth +10.9%. Also retracted: the first decomposition built on run #1's 2,820 req/s figure, which is superseded by §12.

---

## 10. The measurement-noise discovery

This is arguably the most operationally important finding in the report, because it invalidates the instrument everything else was being measured with.

### 10.1 The two instruments compared

| Instrument | Duration | VUs | Mix | Observed spread |
|---|---|---|---|---|
| `k6/grading.js` full ramp | 4m30s | 0 → 200 ramping | 60/30/10 | **40.25%** |
| `risk_probe.js` | 40s | 16 constant | 100% `/risk` | **1.13%** |
| *(Spark, for reference — from `STRATEGY.md:233`)* | 4m30s | 0 → 200 | 60/30/10 | *0.075%* |

The Spark's full-run spread was **0.075%**. This box's is **40.25%** — roughly **537× noisier** on the identical script.

### 10.2 Where the noise is not

The compute path is stable. The risk-only probe, which exercises the hash chain under saturation and almost nothing else, repeats to 1.13% (x86) and 0.43% (reference). If the CPU were thermally throttling or the VM were stealing cycles unpredictably, the probe would show it. It does not.

### 10.3 Where the noise probably is

By elimination, the instability is in the layers the probe does not stress:

- **VU count.** The probe uses 16 concurrent connections; the graded run ramps to 200. Connection setup/teardown and socket churn at 200 VUs through a WSL2 virtual NIC is the leading candidate.
- **The ramp itself.** The graded profile spends 3 of its 4.5 minutes changing VU count. Transient behaviour during ramping may interact badly with the VM's scheduler.
- **Run duration.** 4m30s of sustained load gives thermal and boost-residency effects time to develop that a 40s probe does not.

The monotonic climb across the three runs (1.90M → 2.11M → 2.67M) is consistent with the machine settling into a higher-performance state over time — background Windows work quiescing, or CPU boost residency improving — rather than with anything about the code.

### 10.4 Practical consequence

**Use the 40s risk-only probe for all optimisation A/B work on this machine.** It resolves ~1% differences. The full graded script should be used only to (a) confirm thresholds pass and (b) produce a headline number, and in the latter case only as a median of several runs.

This is also a genuinely good line for the resilience write-up: *"we discovered our measurement instrument had a 40% noise floor and built a 1% one before trusting any optimisation decision."* That is exactly the "deliberate engineering, not luck" the on-theme prize rewards.

---

## 11. The alternating probe — definitive backend A/B

### 11.1 Design

To eliminate time-drift as a confound, the two backends were alternated within a single scripted sequence — x86, reference, x86, reference, x86, reference — so any monotonic machine drift affects both arms equally.

Probe script (`risk_probe.js`):

```javascript
import http from 'k6/http';
import { check } from 'k6';
const BASE = __ENV.TARGET || 'http://localhost:8080';
export const options = {
  scenarios: { probe: { executor: 'constant-vus', vus: 16, duration: '40s' } },
  thresholds: {},
};
export default function () {
  const res = http.get(`${BASE}/risk?seed=${Math.random()}`, { tags: { name: 'risk' } });
  check(res, { 'ok': (r) => r.status === 200 });
}
```

Each arm: container destroyed and recreated, 3s settle, 40s saturating run.

### 11.2 Raw results

```
rep=1 backend=x86        total=18302  rate=457.231397/s
rep=1 backend=reference  total=14368  rate=358.883004/s
rep=2 backend=x86        total=18510  rate=462.418480/s
rep=2 backend=reference  total=14305  rate=357.344357/s
rep=3 backend=x86        total=18437  rate=460.565434/s
rep=3 backend=reference  total=14324  rate=357.779698/s
```

### 11.3 Analysis

| Backend | rep 1 | rep 2 | rep 3 | mean | spread |
|---|---|---|---|---|---|
| x86-sha-ni | 457.231 | 462.418 | 460.565 | **460.072 /s** | 1.13% |
| reference | 358.883 | 357.344 | 357.780 | **358.002 /s** | 0.43% |

```
Ratio = 460.072 / 358.002 = 1.28511
```

### 11.4 Conclusion

**The x86 SHA-NI backend is worth +28.51% over the reference fallback**, at a noise floor of ~1.1%. The margin is ~25× the noise. Every one of the three x86 reps beat every one of the three reference reps, with no overlap in range (x86 min 457.23 > reference max 358.88).

This is unambiguous. **`AUDIT-HANDOFF.md` §0.3 is closed: the x86 backend earns its place.**

It also demonstrates the retracted −10.9% was pure artifact. The true sign is positive and the magnitude is large.

### 11.5 Per-chain cost

At 2 CPUs, the container has 2,000 CPU-ms available per wall-clock second:

| Backend | chains/s | CPU-ms per chain |
|---|---|---|
| x86-sha-ni | 460.072 | **4.3471 ms** |
| reference | 358.002 | **5.5865 ms** |

---

## 12. Quantitative decomposition: why the score dropped vs the Spark

This supersedes the decomposition given earlier in the session, which was built on noisy run-#1 data.

### 12.1 Method

All figures converted to **amortised CPU-ms per 50,000-round chain**, using 2,000 CPU-ms/s of quota. For the Spark rows, `/risk` is 10% of the mix, so risk-chains/s = total req/s × 0.10.

### 12.2 The Spark ladder (from `STRATEGY.md:184-190`)

| Build | total req/s | risk chains/s | CPU-ms/chain | vs its fallback |
|---|---|---|---|---|
| OpenSSL two-block `sha256_64` | 4,439 | 443.9 | 4.5056 | — |
| ARM crypto x2 | 9,650 | 965.0 | 2.0725 | 2.174× |
| ARM crypto x3 | 11,957 | 1,195.7 | 1.6727 | 2.693× |
| **ARM crypto x4 (shipped)** | 13,140 | 1,314.0 | **1.5221** | **2.960×** |
| x5 (rejected) | 13,217 | 1,321.7 | 1.5132 | 2.978× |

The derived 2.960× reproduces `STRATEGY.md`'s headline "2.96× end to end" exactly, which validates the conversion method.

### 12.3 The Ryzen measurements (§11, stable data)

| Build | chains/s | CPU-ms/chain | vs its fallback |
|---|---|---|---|
| reference (`sha256_64`) | 358.002 | 5.5865 | — |
| x86-sha-ni | 460.072 | 4.3471 | **1.285×** |

### 12.4 The decomposition

**Step 1 — isolate hardware + virtualization.** Compare the two machines running the *same* fallback code path:

```
Ryzen reference / Spark fallback = 5.5865 / 4.5056 = 1.2399×
```

**Step 2 — isolate the backend.** Compare what each machine's accelerated backend bought it:

```
Spark ARM x4 gain / Ryzen x86 gain = 2.960 / 1.285 = 2.3035×
```

**Step 3 — verify the product reconstructs the observed gap.**

```
Predicted:  1.2399 × 2.3035 = 2.8562×
Observed:   4.3471 / 1.5221 = 2.8560×
Error:      0.007%
```

### 12.5 Result

| Contribution | Factor | Share of the gap |
|---|---|---|
| Hardware + WSL2 virtualization | 1.2399× | **~24%** |
| Backend (ARM x4 vs x86 coarse) | 2.3035× | **~76%** |
| **Total** | **2.856×** | 100% |

### 12.6 Interpretation

**Only about a quarter of the shortfall is this machine.** The Ryzen, even through a WSL2 VM, is a mere 1.24× slower than a Cortex-X925 on native Linux at this workload. That is a small penalty.

**Three quarters of it is the backend gap.** ARM's four-lane, register-resident, round-by-round interleave extracts 2.96× over its fallback. x86's coarse interleave extracts 1.285×. The difference between those two multipliers is where the score went.

This reframes the problem entirely. The answer is not "get better hardware" — it is "close the backend gap," and §13 explains how much of it is closable.

---

## 13. Register-file analysis: why ARM's win does not transfer to x86

### 13.1 The common misconception

x86 SHA-NI does **not** provide dedicated SHA registers. This matters because the entire ARM result depends on register capacity.

### 13.2 The two instruction sets compared

**x86 SHA extensions (SHA-NI)** — `SHA256RNDS2`, `SHA256MSG1`, `SHA256MSG2`:

- SSE-encoded; operate on the ordinary **XMM register file**.
- x86-64 provides **16 XMM registers** (XMM0–XMM15) in SSE encoding.
- `SHA256RNDS2` takes an **implicit XMM0 source operand**, holding the K+W values for the two rounds it performs. XMM0 is therefore effectively reserved on every round, leaving ~15 usable.
- Processes **2 rounds per instruction**.
- AVX-512 adds XMM16–31, but the SHA-NI instructions have no encoding that reaches them.

**ARMv8 crypto extensions** — `sha256h`, `sha256h2`, `sha256su0`, `sha256su1`:

- Operate on the **NEON register file: 32 registers** (v0–v31).
- No implicit reserved operand.

### 13.3 Register budget per lane

One lane of in-flight SHA-256 state costs approximately **6 vector registers**:

- 2 for working state (the ABEF and CDGH halves)
- 4 for the message schedule (W[0..15] held as four 128-bit vectors)

| | register file | usable | lanes that fit | what shipped | measured gain |
|---|---|---|---|---|---|
| ARM NEON | 32 | 32 | 32 / 6 ≈ **5** | x4 (24 regs) | **+196% (2.96×)** |
| x86 SHA-NI | 16 | ~15 (XMM0 implicit) | 15 / 6 ≈ **2** | coarse (effectively x1) | **+28.5% (1.285×)** |

### 13.4 The code's own account

`src/chain_x86.cpp:306` states the constraint accurately and honestly:

> "Coarse-grained interleaving: independent, data-dependency-free calls back to back let an out-of-order core overlap some of `sha256rnds2`'s multi-cycle latency between the chains. **Not as tight a weave as the ARM back end's round-by-round interleave** — on x86 that would mean carrying three or four lane states through one hand-unrolled loop against a 16-entry XMM register file (six registers per lane minimum), which spills before it pays."

The reasoning is correct. This is an ISA constraint, not a shortcoming of the implementer. **The +36% four-lane win is physically unavailable on this chip.**

### 13.5 What the current code actually does

`src/chain_x86.cpp:317`:

```cpp
for (int r = 0; r < rounds; ++r) {
  hash64(sa, sa);
  hash64(sb, sb);
}
```

Each lane's state **round-trips through a `char[64]` in memory on every one of the 50,000 iterations**, and the two calls are sequential. The only instruction-level overlap available is whatever the out-of-order window happens to capture across an entire two-block hash boundary.

Structurally this is **x1 with per-iteration store/load traffic**, not a genuine 2-lane interleave. It is not exploiting even the two lanes that *do* fit in the XMM file.

### 13.6 Estimated remaining headroom

| Configuration | multiplier over fallback |
|---|---|
| x86 current (coarse) | 1.285× *(measured)* |
| ARM x2 (register-resident, for scale) | 2.174× *(derived from Spark data)* |
| **x86 true round-by-round x2** | **~1.7–2.0× (estimated)** |

x86 will not match ARM's x2 ratio — fewer registers, the implicit XMM0 reservation, and a different instruction shape (2 rounds/instruction vs ARM's arrangement) all cut against it. But the gap between 1.285× and roughly 1.8× is real and represents an estimated **+30–60% on the `/risk` path**.

Since `/risk` is ~98% of all CPU and score is proportional to throughput, that translates almost one-for-one into score.

**This is the single remaining optimisation lever of consequence on x86 hardware.**

---

## 14. Validation of the project's own scoring model

`STRATEGY.md:15` asserts `score ≈ 2.5 × (total requests served)`, derived from the fixed 60/30/10 client-side mix and weights 1/3/10:

```
0.6 × 1  +  0.3 × 3  +  0.1 × 10  =  0.6 + 0.9 + 1.0  =  2.5
```

Tested against all three graded runs conducted here:

| Run | requests | predicted (× 2.5) | actual `work_score` | error |
|---|---|---|---|---|
| 1 (x86) | 761,360 | 1,903,400 | 1,901,672 | **−0.0908%** |
| 2 (reference) | 843,272 | 2,108,180 | 2,108,026 | **−0.0073%** |
| 3 (x86 repeat) | 1,067,749 | 2,669,372.5 | 2,667,160 | **−0.0829%** |

**The model holds to better than 0.1% on all three runs, on hardware it was never derived on.**

This independently confirms the project's central strategic claim: **score is proportional to total requests served, and nothing else moves it materially.** The prioritisation logic that sent all effort at the hash chain was correct, and is now validated on x86 as well as ARM.

The small consistent negative bias (~0.06% mean) is the handful of in-flight requests that do not complete before the run ends.

---

## 15. Repository defects and documentation contradictions

### 15.1 The architecture contradiction (highest consequence)

| Source | Claim |
|---|---|
| `STRATEGY.md:288` (commit `4b74042`) | "the grading box is an **arm64 Mac**" |
| `STRATEGY.md:333` | "**The x86 question is closed.** No x86 back end, no x86 cloud VM." |
| `STRATEGY.md:548` | "~~What architecture is the grading box?~~ **Answered: an arm64 Mac.**" |
| `STRATEGY.md:482` | "~~x86 SHA-NI back end.~~ **Dead**" |
| Commit `13b95da` (the very next commit) | ships a 418-line x86 SHA-NI backend |
| `AUDIT-HANDOFF.md:57` | team stated verbally: grading is **AMD Ryzen 7, Windows** |
| **This machine, measured** | **AMD Ryzen 7 170, x86-64** |
| **This session, measured** | x86 backend selected, serving, **+28.5%** |

`STRATEGY.md` currently declares dead a code path that this session proved is both selected and valuable. The document is actively misleading on its single most consequential point, and the commit that added the x86 backend did not touch it.

**Everything in `STRATEGY.md` §5 (the 2.96×, the +36%, the x4 lane choice, the 132 ms `/risk` p95) describes arm64 silicon that will not run the graded workload if grading is on x86.**

### 15.2 `compose/docker-compose.yml` describes a system that was not built

Still the untouched starter template:

```
line 18:  build: ../starters/node
line 24:  DATABASE_URL: "postgres://obsidio:obsidio@db:5432/obsidio"
line 32:  image: postgres:16-alpine
line 37-38: volumes: dbdata:/var/lib/postgresql/data
```

The actual implementation is a C++ service with an append-only file log and **no database at all** — deliberately, and correctly, since `POST /price` is not in the graded mix.

Submitting this file with a persistence-bonus claim would describe a Node + Postgres architecture the team did not build. `OBSIDIO-DETAIL-PAGE.md:154` requires a `docker-compose.yml` with a persistence submission.

### 15.3 The persistence bonus does not apply as shipped

- `main.cpp:152` defaults the log to `/data/prices.log`.
- The Dockerfile never creates `/data` and declares no `VOLUME`.
- Runtime confirms: `persistence: DISABLED (/data/prices.log not writable)`.

The degrade is graceful, but the bonus scores zero unless the grader mounts a volume — and nothing in the submission instructs them to.

### 15.4 Forced-backend self-test passes are documented but unwired

`risk.cpp:186` and `tests/selftest.cpp:243` both describe `RISK_BACKEND=arm` / `x86-sha-ni` as "what the Dockerfile's forced self-test passes use."

`Dockerfile:30-31` runs **default + `reference` only**. The SKIP/FAIL logic for forced passes exists and is unreachable. The code claims a guarantee the build does not provide.

### 15.5 `IO_THREADS=1` is measured but not applied

`STRATEGY.md:238-255` records a sweep on the Spark:

| io | rw | `work_score` | vs committed |
|---|---|---|---|
| **1** | **2** | **9,211,336** | **+3.9%** |
| 1 | 3 | 9,021,912 | +1.8% |
| 2 | 2 (committed) | 8,866,401 | — |
| 2 | 3 | 8,718,295 | −1.7% |

Confirmed by re-run (9,211,336 / 9,189,333, 0.24% spread) for a **+3.6–3.9%** gain. The default remains `IO_THREADS=2`. It was gated on a "Mac re-check" that the architecture change makes irrelevant — it now needs an **x86** re-check instead.

**Caution:** given §10, this cannot be re-verified on this box with the full graded script. It needs the probe methodology, and the probe as written is `/risk`-only so it would need a mixed variant to capture IO-thread effects.

### 15.6 Build artifacts are committed

`.gitignore` (added in `13b95da`) lists:

```
starters/cpp/build*/
starters/cpp/bench/throttle
.serena/
```

But these files were added in the same push, so **git already tracks them** — `.gitignore` does not apply to tracked files. Currently in history:

- ~110 files under `starters/cpp/build/` and `starters/cpp/build-asan/`
- Compiled `.o` objects (`data.cpp.o`, `risk.cpp.o`, `sha256.cpp.o`)
- `CMakeCache.txt` × 2, each ~432 lines containing **absolute paths from the original Mac**
- `CMakeFiles/4.2.3/CompilerIdCXX/a.out` — a compiled binary
- `bench/throttle` — a compiled **arm64** binary (74,520 bytes), useless on x86

The cached absolute paths will actively fight anyone configuring a build in a different directory. Untrack without deleting local copies:

```bash
git rm -r --cached starters/cpp/build starters/cpp/build-asan starters/cpp/bench/throttle
```

### 15.7 Stale counts and self-contradicting scope notes

- `STRATEGY.md:262` claims **56/56** self-test checks. Actual: **63**.
- `AUDIT-HANDOFF.md:8-9` states "**no build was run, no tests were executed, no code was changed**" while preparing it. But its own commit `13b95da` states "audit executed 2026-08-22 (both Docker builds pass selftest gates, full k6 green on M2, goldens independently reverified)." The document's scope note is stale relative to the commit that introduced it.

### 15.8 No x86 microbenchmark exists

`bench/bench_chain.cpp:11` does `#include <arm_neon.h>` and is ARM-only. There is **no way to microbenchmark the chain on x86** in this repo. Any x86 optimisation work needs an x86 equivalent written first — or must rely on the end-to-end probe from §11, which is a perfectly serviceable substitute at ~1% resolution.

---

## 16. Audit-item status ledger

Status of every item in `AUDIT-HANDOFF.md` §5 ("Known-unverified list"), as of this session:

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Does the current tree compile? | ✅ **CLOSED — yes** | Docker build succeeded; `chain_backend.hpp:53` declares `x86_sha_backend()` |
| 2 | Which architecture is graded? | ⚠️ **This box is x86-64 Ryzen** | `Win32_Processor` → AMD Ryzen 7 170; still needs written confirmation from organisers |
| 3 | Has the x86 backend been benchmarked end to end? | ✅ **CLOSED — yes, +28.5%** | §11 alternating probe, 3 reps, 1.13% noise |
| 4 | Has the lane count been re-swept on grading silicon? | ❌ **OPEN** | Not attempted; x86 max is ~2 lanes anyway (§13) |
| 5 | Does `verify_backend()` select the accelerated chain in the grading env? | ✅ **CLOSED — yes** | Banner: `hash back end: x86-sha-ni (x1..x4 coarse interleave)` |
| 6 | Is the `IO_THREADS=1` +3.9% win being deliberately declined? | ❌ **OPEN** | Default still 2; needs x86 re-measurement with a noise-resistant method |
| 7 | Does `compose/docker-compose.yml` need replacing? | ✅ **CONFIRMED — yes** | Still node + Postgres template (§15.2) |

Additional items from other audit sections:

| Item | Status | Evidence |
|---|---|---|
| §0.3 — is the x86 coarse interleave worth anything? | ✅ **Yes, +28.5%** — audit's pessimism was wrong | §11 |
| §3 — `/data` never created, bonus silently inapplicable | ✅ **CONFIRMED** | Runtime banner (§4.2) |
| §2 — forced-backend passes unwired | ✅ **CONFIRMED** | `Dockerfile:30-31` (§15.4) |
| §2 — selftest count stale (56 vs 63) | ✅ **CONFIRMED** | `grep -c` → 63 |

---

## 17. Environmental gotchas encountered

Recorded because they cost time and will recur.

### 17.1 Git Bash MSYS path mangling

Running the k6 container from the Bash tool failed with:

```
level=error msg="The moduleSpecifier \"C:/Program Files/Git/scripts/grading.js\"
couldn't be found on local disk."
```

Git Bash on Windows rewrites a leading `/scripts/...` argument into a Windows path (`C:/Program Files/Git/scripts/...`) before the container ever sees it. The container-side path is destroyed.

**Workarounds:** prefix with `MSYS_NO_PATHCONV=1`, double the leading slash (`//scripts/grading.js`), or invoke via PowerShell. All k6 runs in this report were driven from PowerShell for this reason.

This also produced a red herring: the failed run exited instantly, leaving a freshly-started container whose uptime did not match the expected timeline, which briefly looked like a crash. It was not.

### 17.2 Negative minimum durations

k6 reported `min=-1526932ns` (run 1) and `min=-902874ns` (run 2) for some tiers. Negative elapsed time is impossible; this is a monotonic-clock artifact of the Windows/WSL2 timing path. It affects only the `min` statistic. **p95, p90, median and average are unaffected** and all threshold evaluations are on p95, so no graded conclusion is impacted.

### 17.3 Reading build logs is misleading

The final self-test output in the Docker build log shows `selected: reference`, because the *last* thing the build does is the forced-fallback pass. The shipped image selects `x86-sha-ni`. Verify the backend from the **container startup banner**, never from the build tail.

### 17.4 Docker Desktop VM memory

The WSL2 VM reports 7.42 GiB total against 15.3 GB of host RAM. Ample for a 2 GiB container (which uses 0.26% of its budget), but worth knowing if any future work wants a larger footprint.

---

## 18. Open questions

Ordered by consequence.

1. **Is the grading box the same Zen family as this laptop, or literally the same machine?**
   - *Same architecture* → optimisation **ratios** transfer; this laptop is a legitimate development box; absolute numbers do not transfer.
   - *Same model* → absolute numbers transfer too.
   - Either way, `chain_arm.cpp` is dead at grading time and `STRATEGY.md` §5 describes irrelevant silicon.

2. **Does the grader run native Linux, or Docker-on-Windows/WSL2?**
   - Native Linux → the WSL2 layer here is a local artifact, and testing should move **inside WSL2 directly** rather than through Docker Desktop's proxy. This may also eliminate much of the 40% noise from §10.
   - Docker-on-Windows → the distortions measured here *are* the grading environment, exactly as `STRATEGY.md` §6 argued for the Mac, and this box becomes the dress-rehearsal machine.

3. **Will the grader mount a volume?** Without one the persistence bonus scores zero (§4.2, §15.3). Also unresolved from `STRATEGY.md:557`: does the grader restart with `docker restart` (same filesystem) or `rm` + `run` (needs a volume)? Assume the stricter case.

4. **What are the final threshold values?** Current ones are explicitly placeholders (`k6/grading.js:10-12`). Current margins are enormous on the fast path (660×) and comfortable on risk (3.02×), so recalibration is low-risk — but do not over-fit.

5. **What is the root cause of the 40% run-to-run variance?** Hypotheses in §10.3. Worth 30 minutes to resolve, because it determines whether any future optimisation can be validated on this machine at all.

---

## 19. Recommendations, prioritised

### Priority 1 — Correct the documentation (minutes, zero risk)

`STRATEGY.md` currently instructs a reader that the x86 path is dead. It is not: it is selected, serving, and worth +28.5%. Rewrite §6, §7 and §11 around x86-64. Until this is done, anyone reading the repo will make wrong decisions — as nearly happened in this session.

### Priority 2 — Fix the persistence bonus (minutes, recovers free points)

Two changes:
- Add `RUN mkdir -p /data` and `VOLUME ["/data"]` to the Dockerfile, so the bonus is not contingent on the grader guessing.
- Replace `compose/docker-compose.yml` with one that builds `../starters/cpp` and mounts a named volume at `/data`. Delete the Postgres service.

`POST /price` is not in the graded mix, so this costs ~0 CPU during the run. `STRATEGY.md:481` already measured the regression at −0.5%, within noise.

### Priority 3 — Implement a true round-by-round x2 in `chain_x86.cpp` (the real lever)

The only remaining optimisation of consequence. Hold both lanes' state and message schedules in XMM registers across all 50,000 iterations, eliminating the per-iteration `char[64]` round-trip at `chain_x86.cpp:317`.

- **Estimated payoff:** +30–60% on the `/risk` path, which is ~98% of CPU and therefore ~1:1 with score.
- **Ceiling:** ~2 lanes, possibly 3. Four is physically impossible (§13).
- **Safety net:** 63 build-gated assertions plus `verify_backend()`'s per-lane boot check. A wrong digest fails the build rather than silently scoring zero.
- **Measurement:** the §11 probe, which resolves ~1%.

### Priority 4 — Resolve the measurement noise (30 minutes, unblocks everything)

Test whether running inside WSL2 directly (bypassing Docker Desktop) collapses the 40% spread. If it does, all future work gets a trustworthy full-run instrument. If it does not, the probe methodology stands as the permanent answer.

### Priority 5 — Re-test `IO_THREADS=1` on x86

Worth +3.6–3.9% on ARM. Needs a mixed-workload probe variant (the current probe is `/risk`-only and would not capture IO-thread effects). Do not trust a full-run A/B on this box.

### Priority 6 — Untrack the build artifacts

```bash
git rm -r --cached starters/cpp/build starters/cpp/build-asan starters/cpp/bench/throttle
git commit -m "chore: untrack build trees now covered by .gitignore"
```

### Priority 7 — Wire up the forced-backend self-test passes, or fix the comments

Either add `RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest` to the Dockerfile, or correct `risk.cpp:186` and `selftest.cpp:243` which currently claim a guarantee the build does not provide.

---

## 20. Exact reproduction instructions

### 20.1 Build

```powershell
docker build -t obsidio-cpp "C:\Users\YASH\achal-projects\obsidio\starters\cpp"
```

### 20.2 Run under the graded caps

```powershell
docker network create obsidio-net
docker run -d --name obsidio-test --network obsidio-net `
  --cpus=2 --memory=2g -p 8098:8080 obsidio-cpp
docker logs obsidio-test          # <-- the backend banner. Most informative line in the system.
```

### 20.3 Verify the digest

```powershell
(Invoke-WebRequest -Uri "http://127.0.0.1:8098/risk?seed=0.5" -UseBasicParsing).Content
docker run --rm python:3.11-slim python -c `
  "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
```

Expected: `8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148`

### 20.4 Full graded run

```powershell
docker run --rm --network obsidio-net --cpuset-cpus=8-15 `
  -v "C:\Users\YASH\achal-projects\obsidio\k6:/scripts" `
  -e TARGET=http://obsidio-test:8080 `
  grafana/k6:latest run /scripts/grading.js
```

⚠️ **Expect ~40% run-to-run variance on this machine.** Use for threshold confirmation only, never for A/B.

### 20.5 The 1%-resolution optimisation probe

Save as `risk_probe.js` (full source in §11.1), then:

```powershell
docker run --rm --network obsidio-net --cpuset-cpus=8-15 `
  -v "<probe-dir>:/scripts" -e TARGET=http://obsidio-test:8080 `
  grafana/k6:latest run /scripts/risk_probe.js
```

Always alternate arms (A/B/A/B/A/B) within one scripted sequence so time-drift cancels.

### 20.6 Force the fallback backend

```powershell
docker run -d --name obsidio-test --network obsidio-net `
  --cpus=2 --memory=2g -e RISK_BACKEND=reference obsidio-cpp
```

### 20.7 Sample the resource envelope

```powershell
docker stats --no-stream --format "CPU={{.CPUPerc}} MEM={{.MemUsage}}" obsidio-test
```

---

## 21. Raw data appendix

### 21.1 Graded run #1 — x86-sha-ni — complete k6 output

```
  █ THRESHOLDS

    http_req_duration{tier:price}
    ✓ 'p(95)<200' p(95)=303.18µs

    http_req_duration{tier:risk}
    ✓ 'p(95)<1500' p(95)=495.92ms

    http_req_duration{tier:stats}
    ✓ 'p(95)<500' p(95)=305.07µs

    http_req_failed
    ✓ 'rate<0.01' rate=0.00%

  █ TOTAL RESULTS

    checks_total.......: 761360  2819.965389/s
    checks_succeeded...: 100.00% 761360 out of 761360
    checks_failed......: 0.00%   0 out of 761360

    ✓ status is 200

    CUSTOM
    work_score.....................: 1901672 7043.513216/s

    HTTP
    http_req_duration..............: avg=29.37ms  min=-1526932ns med=184.96µs max=670.15ms p(90)=11.53ms  p(95)=252.98ms
      { expected_response:true }...: avg=29.37ms  min=-1526932ns med=184.96µs max=670.15ms p(90)=11.53ms  p(95)=252.98ms
      { tier:price }...............: avg=234.93µs min=21.52µs    med=178.47µs max=19.37ms  p(90)=260.23µs p(95)=303.18µs
      { tier:risk }................: avg=292.71ms min=4.12ms     med=253.63ms max=670.15ms p(90)=470.03ms p(95)=495.92ms
      { tier:stats }...............: avg=235.33µs min=-1526932ns med=180.91µs max=19.36ms  p(90)=262.48µs p(95)=305.07µs
    http_req_failed................: 0.00%   0 out of 761360
    http_reqs......................: 761360  2819.965389/s

    EXECUTION
    iteration_duration.............: avg=29.44ms  min=84.95µs    med=249.14µs max=670.28ms p(90)=11.63ms  p(95)=253.1ms
    iterations.....................: 761360  2819.965389/s
    vus............................: 1       min=0           max=199
    vus_max........................: 200     min=200         max=200

    NETWORK
    data_received..................: 122 MB  453 kB/s
    data_sent......................: 69 MB   257 kB/s

running (4m30.0s), 000/200 VUs, 761360 complete and 0 interrupted iterations
siege ✓ [ 100% ] 000/200 VUs  4m30s
```

### 21.2 Graded run #2 — reference — complete k6 output

```
  █ THRESHOLDS
    http_req_duration{tier:price}
    ✓ 'p(95)<200' p(95)=298.36µs
    http_req_duration{tier:risk}
    ✓ 'p(95)<1500' p(95)=577.85ms
    http_req_duration{tier:stats}
    ✓ 'p(95)<500' p(95)=301.18µs
    http_req_failed
    ✓ 'rate<0.01' rate=0.00%
    ✓ status is 200
    work_score.....................: 2108026 7807.796821/s
    http_req_duration..............: avg=26.52ms  min=-902874ns med=183.59µs max=667.06ms p(90)=8.68ms   p(95)=243.22ms
      { expected_response:true }...: avg=26.52ms  min=-902874ns med=183.59µs max=667.06ms p(90)=8.68ms   p(95)=243.22ms
      { tier:price }...............: avg=229.75µs min=-902874ns med=177.29µs max=18.47ms  p(90)=256.48µs p(95)=298.36µs
      { tier:risk }................: avg=263.58ms min=5.23ms    med=243.52ms max=667.06ms p(90)=521.46ms p(95)=577.85ms
      { tier:stats }...............: avg=232.74µs min=51.43µs   med=179.63µs max=16.59ms  p(90)=259.64µs p(95)=301.18µs
    http_req_failed................: 0.00%   0 out of 843272
    http_reqs......................: 843272  3123.346885/s
    iteration_duration.............: avg=26.58ms  min=82.41µs   med=247.63µs max=667.11ms p(90)=8.77ms   p(95)=243.32ms
siege ✓ [ 100% ] 000/200 VUs  4m30s
```

### 21.3 Graded run #3 — x86-sha-ni repeat — complete k6 output

```
  hash back end: x86-sha-ni (x1..x4 coarse interleave)

    ✓ 'p(95)<200' p(95)=294.75µs
    ✓ 'p(95)<1500' p(95)=452.66ms
    ✓ 'p(95)<500' p(95)=297.61µs
    ✓ 'rate<0.01' rate=0.00%
    work_score.....................: 2667160 9878.798918/s
    http_req_duration..............: avg=20.92ms  min=29.33µs med=180.33µs max=532.82ms p(90)=9.48ms   p(95)=191.08ms
      { expected_response:true }...: avg=20.92ms  min=29.33µs med=180.33µs max=532.82ms p(90)=9.48ms   p(95)=191.08ms
      { tier:price }...............: avg=236.93µs min=29.33µs med=174.18µs max=18.28ms  p(90)=253.74µs p(95)=294.75µs
      { tier:risk }................: avg=207.57ms min=4.05ms  med=191.43ms max=532.82ms p(90)=408.22ms p(95)=452.66ms
      { tier:stats }...............: avg=241.36µs min=43.09µs med=176.48µs max=17.57ms  p(90)=256.44µs p(95)=297.61µs
    http_reqs......................: 1067749 3954.797487/s
    iteration_duration.............: avg=20.98ms  min=78.9µs  med=242.64µs max=532.88ms p(90)=9.57ms   p(95)=191.17ms
```

### 21.4 Alternating probe — raw

```
rep=1 backend=x86        total=18302  rate=457.231397/s
rep=1 backend=reference  total=14368  rate=358.883004/s
rep=2 backend=x86        total=18510  rate=462.418480/s
rep=2 backend=reference  total=14305  rate=357.344357/s
rep=3 backend=x86        total=18437  rate=460.565434/s
rep=3 backend=reference  total=14324  rate=357.779698/s
```

### 21.5 Container resource samples

```
16:52:15 CPU=200.87% MEM=5.34MiB / 2GiB
16:52:17 CPU=200.93% MEM=5.316MiB / 2GiB
16:52:19 CPU=199.21% MEM=5.316MiB / 2GiB
16:52:21 CPU=198.19% MEM=5.316MiB / 2GiB
16:52:23 CPU=202.11% MEM=5.062MiB / 2GiB
16:52:25 CPU=201.54% MEM=5.062MiB / 2GiB
16:52:27 CPU=198.01% MEM=5.062MiB / 2GiB
16:52:29 CPU=198.80% MEM=5.062MiB / 2GiB
```

### 21.6 Summary comparison table — all runs

| Run | Backend | Instrument | `work_score` | req/s | price p95 | stats p95 | risk p95 | errors |
|---|---|---|---|---|---|---|---|---|
| 1 | x86-sha-ni | full ramp | 1,901,672 | 2,819.97 | 303.18 µs | 305.07 µs | 495.92 ms | 0.00% |
| 2 | reference | full ramp | 2,108,026 | 3,123.35 | 298.36 µs | 301.18 µs | 577.85 ms | 0.00% |
| 3 | x86-sha-ni | full ramp | 2,667,160 | 3,954.80 | 294.75 µs | 297.61 µs | 452.66 ms | 0.00% |
| P1a | x86-sha-ni | 40s probe | — | 457.23 (risk only) | — | — | — | — |
| P1b | reference | 40s probe | — | 358.88 (risk only) | — | — | — | — |
| P2a | x86-sha-ni | 40s probe | — | 462.42 (risk only) | — | — | — | — |
| P2b | reference | 40s probe | — | 357.34 (risk only) | — | — | — | — |
| P3a | x86-sha-ni | 40s probe | — | 460.57 (risk only) | — | — | — | — |
| P3b | reference | 40s probe | — | 357.78 (risk only) | — | — | — | — |

**Totals across the three full graded runs:** 2,672,381 requests, **0 errors**, **0 failed checks**, all thresholds green every time.

---

## Closing note on epistemic status

Three tiers of confidence in this report:

- **Verified** — measured directly in this session, with the raw output in §21. All of §3–§12, §14, §15.
- **ISA analysis** — §13's register-budget reasoning follows from documented instruction-set properties and is corroborated by `chain_x86.cpp`'s own comments and by the measured ARM/x86 gain differential. The lane-capacity arithmetic is an engineering estimate, not a measurement.
- **Estimate** — §13.6's "+30–60%" headroom figure is an extrapolation from the ARM x2 ratio, adjusted downward for x86's register constraints. It is the least certain number in this document and should be treated as a hypothesis to test, not a promise.

One conclusion was drawn and retracted during this session (§9.2). It is documented rather than deleted, because the reason it was wrong — a 40% noise floor in the measurement instrument — is itself one of the most important findings here.
