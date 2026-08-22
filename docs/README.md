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

The repository's current state is defined by code on `main`, the root README,
and the C++ implementation guide—not by unresolved recommendations in a dated
report.
