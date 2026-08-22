# Audit handoff — Obsidio C++ submission

**Prepared:** 2026-08-22 · **Branch:** `submission` · **Scope:** `starters/cpp/`

This is a briefing for an auditor coming in cold. It describes what the system
is, what is verified, and — the part worth your time — where I think it is
weakest. It is written from a read of the tree only: **no build was run, no
tests were executed, no code was changed** while preparing it. Every claim below
is either quoted from a repo document (attributed) or derived from reading
source (file:line given so you can check it in seconds).

---

## 0. Read this first — three things that need a decision before anything else

### 0.1 The tree looks like it does not compile

`src/risk.cpp:199` calls `chain::x86_sha_backend()`. That function is **defined**
in `src/chain_x86.cpp:401` (and `:413` for the non-x86 branch), but I can find no
**declaration** of it anywhere a translation unit could see:

```
$ grep -rn "x86_sha_backend" src/
src/risk.cpp:199:    candidate = chain::x86_sha_backend();
src/chain_x86.cpp:401:const Backend* x86_sha_backend() {
src/chain_x86.cpp:413:const Backend* x86_sha_backend() { return nullptr; }
```

`src/chain_backend.hpp` — which `risk.cpp` includes — declares only
`arm_crypto_backend()` (line 49). A definition in another TU is not a
declaration, so `risk.cpp` should fail with *"'x86_sha_backend' is not a member
of 'obsidio::chain'"*.

I did not run the build to confirm (see scope note above). **Verify first**, it
takes one command, and if it fails then nothing else in this document has been
exercised on the current tree:

```
cd starters/cpp && docker build -t obsidio-cpp .
```

If it does fail, the fix is one line in `chain_backend.hpp` next to the existing
`arm_crypto_backend()` declaration. The reason this matters beyond the typo: it
means the x86 work in the tree has **not** been through the build gate, so treat
everything in §3 as unverified.

### 0.2 The repo and the team disagree about the grading hardware

This is the single highest-consequence open item, because the entire
optimisation strategy is downstream of it.

| Source | Claim |
|---|---|
| `STRATEGY.md:288` (committed today, `4b74042`) | "**the grading box is an arm64 Mac**" |
| `STRATEGY.md:333` | "**The x86 question is closed.** No x86 back end, no x86 cloud VM. On an arm64 grading box the SHA-NI path can never be selected." |
| `STRATEGY.md:547` | Open question struck through: "~~What architecture is the grading box?~~ **Answered: an arm64 Mac.**" |
| Team, verbally, after that commit | Final grading is on an **AMD Ryzen 7, Windows** |
| Working tree | Contains a new, uncommitted **x86 SHA-NI back end** (`src/chain_x86.cpp`, 418 lines) wired into `CMakeLists.txt` |

So the committed strategy says one thing, the untracked code says the opposite,
and the two most recent human statements conflict. Somebody acted on the Ryzen
information without updating the document, which is exactly the state in which a
wrong assumption survives.

**Nothing else in this audit can be prioritised until this is settled**, because
the two answers point at different code paths:

- **arm64 Mac** → `chain_arm.cpp` serves; `chain_x86.cpp` is dead weight; the
  measured 2.96× in `STRATEGY.md:182` applies (modulo re-measuring on Apple
  silicon, which that document itself flags as not yet done).
- **x86 Ryzen** → `chain_arm.cpp` is dead; `chain_x86.cpp` serves **if** it
  compiles and passes verification; and the headline measurements in
  `STRATEGY.md` describe hardware that will not run the graded workload.

Get it in writing from the organisers and record the answer with a date.

### 0.3 The x86 back end is a different design from the ARM one, and its payoff is unmeasured

`STRATEGY.md:205` states the central finding of this whole project:

> "**Hand-written intrinsics for a single chain are worth ~3%.** ... Essentially
> the entire win is the *interleave*, not the instructions."

The ARM back end interleaves lanes **round by round**, holding every lane's
state in registers for the whole 50,000 iterations. The x86 back end does not.
From its own comment at `src/chain_x86.cpp:306`:

> "Coarse-grained interleaving: independent, data-dependency-free calls back to
> back let an out-of-order core overlap some of sha256rnds2's multi-cycle
> latency between the chains. **Not as tight a weave as the ARM back end's
> round-by-round interleave** — on x86 that would mean carrying three or four
> lane states through one hand-unrolled loop against a 16-entry XMM register
> file (six registers per lane minimum), which spills before it pays."

The reasoning is sound and the comment is commendably honest. But look at what
`chain2_impl` actually does (`src/chain_x86.cpp:317`):

```cpp
for (int r = 0; r < rounds; ++r) {
  hash64(sa, sa);
  hash64(sb, sb);
}
```

Each lane's state round-trips through a `char[64]` in memory every iteration,
and the two calls are sequential. Whatever overlap exists comes only from the
out-of-order window spanning an entire two-block hash. That is a much weaker
mechanism than the ARM path's.

**Consequence:** if the ARM number holds — that intrinsics alone are worth ~3%
and the interleave is everything — then the x86 back end may deliver far less
than the 2.96× the strategy document advertises, possibly close to nothing over
a well-called OpenSSL. **No end-to-end measurement of the x86 path exists in
this repo.** Do not let the ARM figures be quoted for an x86 grading run.

---

## 1. What the system is

A JSON HTTP service on port 8080, four endpoints, scored under a k6 ramp to 200
virtual users at a fixed 60/30/10 mix, in a container capped at 2 CPUs / 2 GB.

| Endpoint | Cost | Notes |
|---|---|---|
| `GET /health` | trivial | not scored |
| `GET /price?symbol=` | in-memory lookup | weight 1 |
| `GET /stats?symbol=` | mean/min/max/stddev over 500 points | weight 3 |
| `GET /risk?seed=` | **50,000-round SHA-256 chain** | weight 10 |
| `POST /price` | in-memory + append-only log | persistence bonus, not in the graded mix |

The governing arithmetic (`STRATEGY.md:11`): the client fixes the mix, so every
100 requests is worth 250 points, and `/risk` is ~98% of all CPU. Therefore
**score is proportional to hash-chain throughput** and nothing else moves it
materially. `STRATEGY.md:32` reports this model predicting 32,850 points/s
against 32,838 measured — worth spot-checking, since it is load-bearing for
every prioritisation decision in the project.

### Architecture

- **`src/http_server.cpp`** — dependency-free epoll HTTP/1.1 server,
  `SO_REUSEPORT` per IO thread, `TCP_NODELAY`.
- **`/price`, `/stats`, `/health`** are answered inline on the IO thread
  (microseconds).
- **`/risk`** is never run on an IO thread. It goes to a bounded queue
  (`src/risk_pool.cpp`); the connection leaves the epoll set and is handed back
  via `eventfd`, so a connection is only ever touched by one thread.
- **Hash workers run at `SCHED_IDLE`** (`src/risk_pool.cpp:26`), with a
  `nice(19)` fallback at `:28`.
- **Thread counts are explicit**, never from `hardware_concurrency()` — the
  container sees all host cores but may use two.
- **Lane batching**: a worker takes up to 4 queued jobs and runs them
  interleaved, with x3/x2/x1 paths for shallower queues
  (`src/risk_pool.cpp`, the `live == 4 … live == 1` ladder).

### Defaults (`Dockerfile:41`)

`IO_THREADS=2`, `RISK_WORKERS=2`, `RISK_QUEUE=512`, `RISK_DEADLINE_MS=0`.

Note `STRATEGY.md:238` records a sweep where **`IO_THREADS=1` beat the committed
`2` by +3.6–3.9%**, confirmed across re-runs with a 0.24% spread — and the
default was deliberately not flipped, pending a re-check. That is a defensible
call, but it is a known ~4% left on the table; confirm it was a decision and not
an oversight.

---

## 2. The correctness story — the strongest part of the build

A wrong digest still looks like a valid 64-character hex string and scores zero
silently. The defences against that are, in my view, the best-engineered part of
this submission and worth auditing *for real* rather than assuming:

1. **An oracle that is deliberately slow.** `src/risk.cpp:14` keeps a plain
   per-iteration reference chain whose only job is to be obviously correct, with
   an explicit "do not optimise this" instruction. It never serves a request.
2. **Startup self-verification.** `select_backend()` (`src/risk.cpp:186`) will
   not select a back end until `verify_core()` reproduces reference digests, and
   verifies **each lane count separately** — `verify_lane3`/`verify_lane4`. A
   failing wide lane is nulled and the pool degrades to fewer lanes rather than
   serving wrong answers (`src/risk.cpp:213-218`).
3. **Build-time gate.** `Dockerfile:30` runs the self-test twice — once on
   whatever back end the build host offers, once with `RISK_BACKEND=reference`
   so the fallback path cannot ship untested.
4. **63 `expect_eq` assertions** in `tests/selftest.cpp`, including FIPS 180-4
   vectors, rotated seed order across lanes, all-same-seed, and cross-checks
   that x4 agrees with x3/x2/x1 on shared lanes. That last class is the
   important one: without it a served digest could depend on how deep the queue
   happened to be.

`STRATEGY.md:257` claims 56/56 passing, ASan+UBSan clean, and peak memory of
6.1 MiB. The check count in the tree is now 63, so that figure is already stale.

### Where I would probe this

- **The oracle and the fast path now share `sha256_64()`.** Commit `4a00b7b`
  ("route the no-backend fallback through sha256_64") means the *serving*
  fallback and the *verifying* oracle may share a primitive. `src/risk.cpp:44`
  acknowledges exactly this risk in a comment. Confirm the oracle still uses the
  generic `sha256()` and not `sha256_64()`, or a bug in `sha256_64` moves both
  sides and verification passes anyway. The independent Python goldens in the
  self-test are the backstop — check they cover the full 50,000-round chain and
  not only short ones.
- **The forced-back-end passes are documented but not used.** `src/risk.cpp:186`
  and `tests/selftest.cpp:243` both describe `RISK_BACKEND=arm` /
  `x86-sha-ni` as "what the Dockerfile's forced self-test passes use". The
  Dockerfile does **not** do this — it runs default + `reference` only
  (`Dockerfile:30`). The SKIP/FAIL logic for forced passes exists and is
  unreached. Either wire it up or correct the comments; right now the code
  claims a guarantee the build does not provide.

---

## 3. Persistence bonus (uncommitted, `src/persist.cpp`)

Single container, append-only text log on a volume, replayed at startup. No
database — correct call, since `POST /price` is not in the graded mix, so
durability only has to be cheap.

Good decisions worth crediting: `fdatasync` per write with the reasoning stated
(`persist.cpp:44` — the grader may `docker kill`, so page cache is not
durability); reliance on small `O_APPEND` writes being atomic to avoid a lock;
torn final line handled by `fscanf` simply failing to match; unknown symbols
rejected via `update_price`. `tests/persist_test.sh` tests the **stricter**
grading case (`docker rm` + fresh run + `docker kill`, volume only).

### Concerns

- **The error budget is the real risk here, not CPU.** Restart downtime counts
  against k6's 1% `http_req_failed` ceiling. At the measured ~13,140 req/s over
  a ~4.5 min run, 1% is roughly 2.7 seconds of *total* downtime. Confirm the
  shutdown path exits promptly — `main.cpp:38` looks right (signal handler does
  async-signal-safe flag sets only, joining happens in `main`) — and confirm
  the listener is bound before log replay so connects queue rather than refuse.
- **`compose/docker-compose.yml` does not match the implementation.** It is
  still the untouched starter template: it builds `../starters/node` and stands
  up **Postgres**. The detail page asks for a `docker-compose.yml` with a
  persistence-bonus submission. Submitting this one would describe a system the
  team did not build.
- **`Dockerfile` never creates `/data`** and declares no `VOLUME`. The default
  path is `/data/prices.log` (`main.cpp:152`). Without `-v`, `persist_init`
  fails and logs "persistence: DISABLED" — a graceful degrade, but it means the
  bonus silently does not apply unless the grader mounts a volume. Verify the
  submission instructions actually cause one to be mounted.

---

## 4. Measurement quality

Better than typical. `STRATEGY.md:231` reports a 0.075% spread across two full
runs, which is what justifies calling x5's +0.63% real-but-small. Lane choice
was made by building and measuring x5 rather than arguing about it, and the
register-file reasoning (six 128-bit vectors per lane, 24 of 32 NEON registers
at x4) is stated and falsifiable.

Two things to be careful about when reading their numbers:

- **Baseline drift.** `STRATEGY.md:191` is explicit that the "2.96× end to end"
  baseline row already includes `sha256_64()`, and that against a naive
  one-shot `SHA256()` build the honest figure is "closer to **10×**" — but that
  10× was never measured end to end. Both numbers are defensible; quoting them
  interchangeably is not. Make sure the write-up picks one baseline and states
  the measurement scope once.
- **All headline figures are from the Spark (aarch64).** If §0.2 resolves to
  x86, none of them describe the graded run.

---

## 5. Known-unverified list

Ordered by how much I would worry:

1. Does the current tree compile? (§0.1)
2. Which architecture is graded? (§0.2)
3. Has the x86 back end ever been benchmarked end to end? (§0.3)
4. Has the lane count been re-swept on the actual grading silicon?
   `STRATEGY.md:314` lists this as an open Mac task; x4 was tuned on
   Cortex-X925 and the doc itself warns the optimum may move.
5. Does `verify_backend()` actually select the accelerated chain inside the
   grading environment? `STRATEGY.md:309` calls this the "highest-priority item
   on the board: it protects the entire 2.96×". The startup banner prints the
   selected back end — one container start and a log read settles it.
6. Is the `IO_THREADS=1` +3.9% win being deliberately declined? (§1)
7. Does `compose/docker-compose.yml` need to be replaced? (§3)

---

## 6. Design questions I would ask, not defects

These are judgement calls where I think the team's reasoning is sound but the
tradeoff deserves a second opinion:

- **`SCHED_IDLE` on the hash workers** (`risk_pool.cpp:26`). It solves
  head-of-line blocking at the kernel rather than in application code. But the
  workers share `mutex_` with `submit()`, which runs on a **normal-priority** IO
  thread (`risk_pool.cpp:52`) — a lowest-priority thread can hold a lock a
  normal-priority thread needs, and only gets rescheduled when nothing else
  wants the CPU. The critical section is short, so this is a tail-latency
  question, not a throughput one. Worth asking whether `SCHED_IDLE` is still
  earning its keep, given `/risk` never runs on an IO thread anyway and
  `/price` p95 is ~0.5 ms against a 200 ms bar.
- **Batch-commit granularity.** A worker commits to up to 4 chains before it
  yields; `STRATEGY.md:224` measures `/price` p95 rising ~22 µs per lane. Fine
  at current margins, and the doc notes latency "is the number that binds
  first" — worth confirming that still holds if thresholds are recalibrated
  downward.
- **No percent-decoding of query parameters** (`main.cpp:57`), deliberate and
  documented. Correct for the known grader; a hidden-test change would break it.

---

## 7. How to reproduce

```
cd starters/cpp
docker build -t obsidio-cpp .                      # self-test gates the build
docker run --rm --cpus=2 --memory=2g -p 8080:8080 obsidio-cpp
# the startup banner names the selected hash back end -- that line is the
# single most informative diagnostic in the system

RISK_BACKEND=reference ...                         # force the fallback, for A/B
./tests/persist_test.sh                            # restart-survival, needs docker
k6 run -e TARGET=http://127.0.0.1:8080 k6/grading.js
```

Uncommitted at time of writing: `src/chain_x86.cpp`, `src/persist.{cpp,hpp}`,
`tests/persist_test.sh` are untracked; eight tracked files are modified. None of
the x86 or persistence work is in a commit, so `git stash`/checkout would lose
it. Worth committing before the audit proceeds so there is a stable object to
review.
