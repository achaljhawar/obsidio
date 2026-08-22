# Documentation

The documentation is divided by authority and time. Read current operational
guides first; use the history directory as evidence, not as instructions.

## Current guides

- [Project README](../README.md) — repository status, quick start, architecture,
  and known limitations.
- [C++ implementation guide](../starters/cpp/README.md) — build, runtime design,
  configuration, testing, persistence, and backend selection.
- [Resilience write-up](resilience-writeup.md) — concise submission narrative,
  controlled measurements, decisions, and trade-offs.
- [Grading script](../k6/grading.js) — executable workload currently checked
  into the repository.

## Challenge source material

These documents preserve the supplied rules and event framing. Where prose and
the executable script differ, both are called out in the current guides.

- [Detailed specification](challenge/spec.md)
- [Directions and resources](challenge/directions.md)
- [Track overview](challenge/track.md)

## Historical engineering record

These are dated snapshots. They deliberately retain measurements, incorrect
hypotheses, and later retractions so the decision trail remains auditable.

- [ARM strategy notes](history/arm64-strategy-notes.md) — early plan and ARMv8
  measurements; its claim that grading would be ARM is superseded.
- [x86 coarse audit](history/x86-coarse-audit.md) — native x86 audit before the
  register-resident two-lane kernel landed.
- [x86 session findings](history/x86-session-findings.md) — later native x86
  measurements, fused-kernel comparison, rejected AVX2 path, and thermal
  analysis.
- [Next levers](history/next-levers.md) — post-fusion idea list; its thermal-
  headroom lever is retracted by the power correction below.
- [Power correction](history/power-correction.md) — retracts the 1.49 GHz
  throttle reading in the session findings as an instrument artifact, and
  gives a portable effective-clock probe.
- [Score levers](history/score-levers.md) — cost model re-derived from
  `k6/grading.js` and the C++ source; supersedes the lever ranking in the next-
  levers note and lists the corrections it implies elsewhere.
- [Ryzen ceiling findings](history/ryzen-ceiling-findings.md) — the
  `perf/ryzen-ceiling` measurement session: `SHA256RNDS2` is latency bound, not
  port bound; scheduler tuning and `IO_THREADS=1` closed negative; two harness
  bugs caught. Corrects five entries in the score-levers note.

## Open design work

Not history — these describe work that is live, and the second retracts the
first stage of the first.

- [Phase-split kernel](phase-split-kernel.md) — the design brief for the wider
  SHA-NI kernel, carrying its own Stage 1 retraction banner.
- [Wide block 2, negative result](wide-block2-negative-result.md) — Stage 1 was
  built and measures −23%; the diagnostic that explains why is what Stage 2 has
  to measure first.
- [Headroom plan](../HEADROOM-PLAN-2026-08-23.md) — the phased plan of record
  derived from all three, with a numeric kill gate at every phase boundary.
- [How to test](../HOW-TO-TEST.md) — the runbook the Ryzen numbers came from.

The repository's current state is defined by code on `main`, the root README,
and the C++ implementation guide—not by unresolved recommendations in a dated
report.
