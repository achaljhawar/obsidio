# Obsidio resilience write-up

## Executive summary

Obsidio has three inexpensive-to-expensive request tiers sharing a two-CPU
container. The failure mode is head-of-line blocking: if 50,000-round SHA-256
chains occupy the same execution path as price lookups, cheap requests wait
behind heavy work and fail their latency target.

The submission prevents that failure in two layers:

1. Linux epoll threads retain ownership of connections and serve `/health`,
   `/price`, and `/stats` directly. `/risk` work moves to a bounded,
   low-priority worker pool and returns through an `eventfd` completion path.
2. The risk workers use runtime-selected ARMv8 SHA2 or x86 SHA-NI kernels that
   interleave independent chains, hiding instruction latency without adding
   threads or exceeding the two-CPU budget.

Correctness is protected by an independent reference chain, startup backend
verification, cross-lane tests, and four Docker build gates. The main remaining
risks are HTTP input hardening, persistence ordering/error handling, and missing
integration CI; none is hidden as completed work.

## What the workload rewards

The client chooses a fixed 60/30/10 mix of price, stats, and risk requests with
weights 1, 3, and 10. Over a sufficiently large run:

```text
expected score per 100 requests = 60×1 + 30×3 + 10×10 = 250
score ≈ 2.5 × completed requests
```

The historical runs reproduced that relationship to better than 0.1 percent.
Almost all CPU time is consumed by `/risk`, while the fast endpoints have large
compute margin. The strategy therefore became:

- keep risk work out of the fast request path;
- reduce amortized CPU time per risk chain;
- preserve exact digests while changing instruction scheduling;
- avoid optimizations that spend correctness or the one-percent error budget.

## Resilience decisions

| Pressure or failure mode | Design choice | Trade-off |
| --- | --- | --- |
| Hash work blocks cheap requests | Dedicated risk pool at `SCHED_IDLE`, with `nice(19)` fallback | Linux-specific scheduling behavior |
| Container sees more CPUs than its quota | Explicit defaults of two IO threads and two risk workers | Requires measurement before retuning on other quotas |
| Unlimited heavy work consumes memory and latency | Bounded queue with optional queue-age deadline | Rejection spends the challenge error budget |
| Worker completion races socket handling | Each socket remains owned by one epoll loop; workers return results through `eventfd` | More custom networking code to test and maintain |
| SIMD optimization silently changes digests | Plain oracle, startup backend verification, cross-lane tests, forced build passes | Additional image-build time |
| CPU architecture differs between machines | Translation-unit-scoped instruction flags plus runtime feature gates | Separate ARM and x86 kernels |
| Container is killed after an update | Append-only log and `fdatasync` when writable | Current failure reporting and concurrent ordering are incomplete |

The queue is deliberately deep by default. Fast shedding can protect latency,
but the challenge permits less than one percent errors; rejection is therefore
a narrow emergency mechanism, not the main throughput strategy.

## Hash-chain optimization

The first SHA-256 operation accepts the arbitrary seed. Every later operation
hashes exactly 64 hexadecimal bytes, so the hot path uses a specialized
two-block transform and SIMD hexadecimal conversion.

A single chain is dependency-bound: the next hash cannot start until the
current digest exists. Modern SHA instructions have greater latency than issue
throughput, leaving execution capacity unused when only one chain is active.
The kernels advance independent requests in lockstep so one lane can issue while
another waits.

### ARMv8

The ARM kernel progressed from two to three to four register-resident lanes.
The four-lane version measured a `work_score` of 8,866,401 versus 2,997,122 for
the OpenSSL baseline on the ARM test system—a 2.96× end-to-end result—with
`/risk` p95 at 132 ms and zero observed errors in that run.

Five lanes produced only a small further gain and increased register pressure
and fast-path latency, so it was measured and rejected.

### x86-64

The first native x86 comparison established that the original SHA-NI backend
was useful: 460.07 risk chains/s versus 358.00 for the reference fallback,
or +28.5 percent, across an alternating probe with roughly one-percent spread.

The original multi-lane functions were only coarse batching: each complete
hash ran sequentially and round-tripped state through memory. A fused two-lane
kernel instead keeps both states and message schedules in XMM registers across
all 50,000 rounds. The controlled probe measured 43,866 completed requests
versus 18,359 for the coarse implementation, a 2.389× risk-path improvement.

Two independently developed fused kernels converged on the same structure. In
head-to-head tests the kernel retained on `main` and the alternate branch were
within 1.2 percent on the risk probe and 2.0 percent on the graded workload,
inside observed noise. The main implementation was kept and the alternate
implementation's lasting value is its measurement evidence.

The x86 register file explains the lane choice: two states plus schedules and
scratch consume almost all 16 XMM registers, including the implicit `XMM0`
operand required by `SHA256RNDS2`. The implementation uses one fused pair for
two lanes, pair-plus-single for three, and two fused pairs for four.

## Measurement discipline

The most important process finding was that the full graded run was a poor A/B
instrument on the Windows/WSL2 laptop. The same image moved from approximately
5.8 million to 2.66 million points as the CPU throttled; `/price` slowed by the
same factor as `/risk`, showing a machine-wide clock change rather than a kernel
regression.

Optimization decisions therefore use tightly alternating short probes and
ratios, not isolated absolute scores. This reduced comparison spread from tens
of percent to roughly one percent. Absolute results are always labelled with
their hardware and thermal context.

That discipline also prevented two expensive dead ends:

- A 256-bit AVX2 multi-buffer hybrid was rejected after a controlled benchmark
  exposed severe AVX/SSE transition costs when mixed with legacy-encoded
  `SHA256RNDS2`.
- More workers were rejected because the service already saturates its exact
  two-CPU quota; extra runnable threads add scheduling contention rather than
  capacity.

## Correctness and degradation

The optimized chain is deterministic but a wrong result still looks like a
valid 64-character digest. Correctness is therefore a build and startup gate,
not an assumption:

- FIPS SHA-256 vectors protect the primitive.
- The specialized 64-byte transform is compared with the generic primitive.
- Short and full 50,000-round goldens protect the chain.
- Rotated, repeated, and cross-width lane cases detect state crossing.
- Accelerated entry points verify against an independent oracle at startup.
- Docker builds test automatic, reference, ARM, and x86 selections; unavailable
  instruction sets skip, while a failure on advertised hardware breaks the
  build.

If an accelerated core fails startup verification, the service falls back to a
correct implementation. If only a wider lane fails, that lane width is disabled
and the pool continues with narrower verified functions. This favors correct,
slower service over silently wrong throughput.

## Persistence status

An append-only log can replay accepted price updates after a fresh container is
started with the same named volume. The restart test exercises `POST`, forced
container termination, removal, recreation, and subsequent `GET`.

The bonus is not claimed as complete in the current repository. The image does
not declare or provision a volume, and without a writable `/data` mount the
service intentionally degrades to in-memory updates. More importantly, the
memory update currently precedes the durable append, write and `fdatasync`
errors are not returned to the caller, and concurrent update ordering is not a
single transaction. Those semantics must be fixed before describing the path
as durable under all tested failures.

## Remaining work

The next work is correctness and operational hardening rather than speculative
hash optimization:

1. Make price mutation and log append one ordered operation with explicit
   failure propagation.
2. Make the grading script validate response shapes, digests, and per-request
   tier latency before awarding work.
3. Extend integration coverage to overload, shutdown, persistence failure,
   restart, and concurrent updates.
4. Re-evaluate `IO_THREADS=1` on native x86 with an alternating mixed-workload
   probe.

Items 1 and 2 of the original list are now closed. `Content-Length` is parsed
with a saturating accumulator that cannot overflow, header and body sizes are
capped, conflicting duplicates and `Transfer-Encoding` are rejected, and
non-finite prices are refused both at the HTTP edge and in `update_price`. The
same change fixed two backpressure defects found while writing the tests: a
`Connection: close` response could be abandoned after a partial write (observed
dropping all 211 bytes of a response), and pipelined requests queued behind a
backpressured write were never parsed at all. Linux CI now runs the image build,
CTest, and an ASan+UBSan pass; `starters/cpp/tests/http_test.cpp` carries 56
socket-level checks and also runs as a Docker build gate.

The measurement lesson repeated itself here. The partial-write path is
unreachable over loopback with default kernel tuning, because autotuning grows
the send buffer to megabytes while the largest response this service can emit is
bounded by the 16 KiB header cap. The test pins `SO_SNDBUF` small so
backpressure is reproducible; without that the test would have passed against
the bug it was written to catch, which is exactly what it did on the first
attempt.

## Evidence

- [ARM strategy notes](history/arm64-strategy-notes.md)
- [x86 coarse audit](history/x86-coarse-audit.md)
- [x86 session findings](history/x86-session-findings.md)
- [C++ implementation guide](../starters/cpp/README.md)
- [Challenge specification](challenge/spec.md)
