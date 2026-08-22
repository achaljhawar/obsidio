#!/usr/bin/env bash
# Thread-configuration sweep, item 2 of the Ryzen plan.
#
# Runs the three candidates through the alternating harness in one sequence so
# they share the same thermal envelope:
#
#   IO_THREADS=2 RISK_WORKERS=2   committed default (Dockerfile)
#   IO_THREADS=1 RISK_WORKERS=2   primary candidate (+3.6-3.9% on ARM)
#   IO_THREADS=1 RISK_WORKERS=3   sanity check
#
# RISK_QUEUE and RISK_DEADLINE_MS are deliberately left at their defaults.
#
# The mixed probe is not optional here. /risk is handed to the worker pool and
# never runs on an IO thread, so a risk-only load leaves the epoll loops idle
# and every IO_THREADS value measures identically. The ARM result this is
# re-testing was a full-mix result, and only a mix can reproduce it.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

exec "$HERE/ab.sh" --probe mixed "$@" \
  "io2rw2:IO_THREADS=2,RISK_WORKERS=2" \
  "io1rw2:IO_THREADS=1,RISK_WORKERS=2" \
  "io1rw3:IO_THREADS=1,RISK_WORKERS=3"
