#!/usr/bin/env bash
# Price the latency insurance.
#
# The hash workers run at SCHED_IDLE, scheduler weight 3 against a normal
# thread's 1024. While any IO thread is runnable the workers get roughly 0.3%
# of the CPU -- effectively nothing. That is why the risk-only probe reaches
# ~1097 chains/s while the graded mix sustains ~849 with the queue never empty:
# the missing 22.6% is capacity handed to the IO threads by the scheduler.
#
# What it buys is /price p95 of 336 microseconds against a 200 ms bar. 595x of
# headroom, paid for with about a fifth of the score.
#
# This sweeps that trade. The mixed probe is mandatory -- /risk never runs on
# an IO thread, so under a risk-only load the epoll loops are idle and every
# scheduling class measures identically.
#
# Read two columns together:
#   score      higher is better, and is the thing being optimised
#   price_p95  must stay far under 200 ms; anything below ~10 ms is still a
#              20x margin, so a large score gain for a few ms is a good trade
#
# The winner is the highest score whose price p95 still leaves comfortable
# margin -- not simply the highest score.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "Sweeping RISK_SCHED. Watch score against price_p95, not score alone."
echo

exec "$HERE/ab.sh" --probe mixed "$@" \
  "idle:RISK_SCHED=idle" \
  "nice19:RISK_SCHED=19" \
  "nice10:RISK_SCHED=10" \
  "nice5:RISK_SCHED=5" \
  "batch:RISK_SCHED=batch" \
  "nice0:RISK_SCHED=0"
