#!/usr/bin/env bash
# Alternating A/B harness for the Ryzen grading box.
#
# Three things this does that a pair of back-to-back k6 runs does not:
#
#   1. Alternates the arms (A B A B A B) inside one sequence, so the monotonic
#      machine drift that made three identical runs score 1.90M, 2.11M and
#      2.67M cancels instead of being attributed to whichever arm ran last.
#   2. Records an effective-clock sample around every arm. The same image on
#      the same machine scored 5.78M cool and 2.66M hot -- a 2.68x clock drop,
#      not a code regression. A measurement taken while the clock is sliding
#      is worthless, and this makes that visible rather than silent.
#   3. Runs k6 outside the capped container, pinned to different CPUs, over a
#      user-defined bridge network -- so the load generator never competes with
#      the service and the Windows port proxy is out of the path.
#
# Usage:
#   ./ab.sh [options] "name:ENV=VAL[,ENV=VAL...]" "name:ENV=VAL[,...]" ...
#
# Examples:
#   # Does the hoisted-constant kernel actually pay on this silicon?
#   ./ab.sh "baseline:RISK_X86_KERNEL=baseline" "hoisted:RISK_X86_KERNEL=hoisted"
#
#   # What is the accelerated back end worth here?
#   ./ab.sh "sha-ni:" "fallback:RISK_BACKEND=reference"
#
#   # IO thread question -- needs the mixed probe, see --probe
#   ./ab.sh --probe mixed "io2:IO_THREADS=2" "io1:IO_THREADS=1"
set -uo pipefail

IMAGE="${IMAGE:-obsidio-cpp}"
REPS="${REPS:-3}"
DURATION="${DURATION:-40s}"
VUS="${VUS:-}"
COOLDOWN="${COOLDOWN:-20}"
PROBE="risk"
NET="obsidio-ab-net"
CTR="obsidio-ab-sut"
SUT_CPUS="${SUT_CPUS:-2}"
# k6 must not share cores with the service. Override for a different topology.
K6_CPUSET="${K6_CPUSET:-8-15}"
CLOCK_FLOOR_PCT="${CLOCK_FLOOR_PCT:-85}"

while [ $# -gt 0 ]; do
  case "$1" in
    --reps)     REPS="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --vus)      VUS="$2"; shift 2 ;;
    --cooldown) COOLDOWN="$2"; shift 2 ;;
    --probe)    PROBE="$2"; shift 2 ;;
    --image)    IMAGE="$2"; shift 2 ;;
    -h|--help)  sed -n '2,30p' "$0"; exit 0 ;;
    *)          break ;;
  esac
done

if [ $# -lt 2 ]; then
  echo "error: need at least two arms; see --help" >&2
  exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
case "$PROBE" in
  risk)  SCRIPT="risk_probe.js" ;;
  mixed) SCRIPT="mixed_probe.js" ;;
  *)     echo "error: --probe must be risk or mixed" >&2; exit 2 ;;
esac

command -v docker >/dev/null || { echo "error: docker not found" >&2; exit 2; }
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
  echo "error: image '$IMAGE' not found. Build it first:" >&2
  echo "  docker build -t $IMAGE starters/cpp" >&2
  exit 2
}

# --- effective clock ---------------------------------------------------------
# A fixed amount of integer work, timed. This is deliberately not /proc/cpuinfo:
# under WSL2 that reports a static nominal figure and never sees the throttle.
# The absolute number is meaningless; the ratio against the first sample is the
# thermal signal, and that is what matters here.
clock_sample() {
  python3 - <<'PY' 2>/dev/null || echo "0"
import time
t0 = time.perf_counter()
x = 0
for i in range(3_000_000):
    x = (x * 1103515245 + 12345) & 0xFFFFFFFF
t1 = time.perf_counter()
print(f"{3.0/(t1-t0):.3f}")
PY
}

# Real temperature if the kernel exposes it. Usually absent under WSL2, in
# which case the clock proxy above is the only signal available -- say so
# rather than printing a fake zero.
temp_sample() {
  local t
  for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$z" ] || continue
    t=$(cat "$z" 2>/dev/null) || continue
    case "$t" in ''|*[!0-9]*) continue ;; esac
    awk -v v="$t" 'BEGIN{printf "%.1f", v/1000}'
    return
  done
  for h in /sys/class/hwmon/hwmon*/temp1_input; do
    [ -r "$h" ] || continue
    t=$(cat "$h" 2>/dev/null) || continue
    case "$t" in ''|*[!0-9]*) continue ;; esac
    awk -v v="$t" 'BEGIN{printf "%.1f", v/1000}'
    return
  done
  echo "n/a"
}

cleanup() {
  docker rm -f "$CTR" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup
docker network create "$NET" >/dev/null 2>&1 || true

start_sut() {
  local envspec="$1"
  local args=()
  if [ -n "$envspec" ]; then
    local IFS=','
    for kv in $envspec; do
      [ -n "$kv" ] && args+=(-e "$kv")
    done
  fi
  docker rm -f "$CTR" >/dev/null 2>&1 || true
  docker run -d --name "$CTR" --network "$NET" \
    --cpus="$SUT_CPUS" --memory=2g ${args[@]+"${args[@]}"} "$IMAGE" >/dev/null || return 1
  for _ in $(seq 1 60); do
    if docker run --rm --network "$NET" curlimages/curl:latest \
         -sf "http://$CTR:8080/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  echo "  ! server never became healthy" >&2
  return 1
}

run_probe() {
  local extra=()
  [ -n "$VUS" ] && extra+=(-e "VUS=$VUS")
  docker run --rm --network "$NET" --cpuset-cpus="$K6_CPUSET" \
    -v "$HERE:/scripts:ro" \
    -e "TARGET=http://$CTR:8080" -e "DURATION=$DURATION" ${extra[@]+"${extra[@]}"} \
    grafana/k6:latest run --quiet "/scripts/$SCRIPT" 2>/dev/null \
    | grep '^PROBE_RESULT'
}

field() { echo "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2; }

echo "image=$IMAGE probe=$PROBE duration=$DURATION reps=$REPS cooldown=${COOLDOWN}s"
echo "sut_cpus=$SUT_CPUS k6_cpuset=$K6_CPUSET"
echo

BASE_CLOCK=""
RESULTS_FILE=$(mktemp)

for rep in $(seq 1 "$REPS"); do
  for arm in "$@"; do
    name="${arm%%:*}"
    envspec="${arm#*:}"
    [ "$envspec" = "$arm" ] && envspec=""

    [ "$COOLDOWN" -gt 0 ] && sleep "$COOLDOWN"

    clk_before=$(clock_sample)
    [ -z "$BASE_CLOCK" ] && BASE_CLOCK="$clk_before"

    if ! start_sut "$envspec"; then
      echo "rep=$rep arm=$name START FAILED"
      continue
    fi
    # Confirm what actually got selected rather than trusting the env var:
    # a mistyped RISK_X86_KERNEL silently serves the default, and an A/B
    # against yourself always reports "no difference".
    backend=$(docker logs "$CTR" 2>&1 | sed -n 's/.*hash back end: //p' | head -1)
    if [ "$rep" = "1" ]; then
      printf "  arm=%-10s serves: %s\n" "$name" "${backend:-unknown}"
    fi

    out=$(run_probe)
    clk_after=$(clock_sample)
    temp=$(temp_sample)

    if [ -z "$out" ]; then
      echo "rep=$rep arm=$name PROBE FAILED"
      continue
    fi

    if [ "$PROBE" = "risk" ]; then
      metric=$(field "$out" rate); label="rate"
    else
      metric=$(field "$out" score); label="score"
    fi
    fail=$(field "$out" fail_rate)

    drift=$(awk -v a="$clk_after" -v b="$BASE_CLOCK" \
              'BEGIN{ if (b>0) printf "%.1f", 100*a/b; else print 0 }')
    warn=""
    if awk -v d="$drift" -v f="$CLOCK_FLOOR_PCT" 'BEGIN{exit !(d < f)}'; then
      warn="  <-- CLOCK DOWN ${drift}% of first sample; treat as throttled"
    fi

    printf "rep=%d arm=%-10s %s=%-12s fail=%s%%  clock=%s%%  temp=%s%s\n" \
      "$rep" "$name" "$label" "$metric" "$fail" "$drift" "$temp" "$warn"
    echo "$name $metric $fail" >> "$RESULTS_FILE"
  done
done

docker rm -f "$CTR" >/dev/null 2>&1 || true

echo
python3 - "$RESULTS_FILE" <<'PY'
import sys, statistics
from collections import OrderedDict
rows = OrderedDict()
fails = OrderedDict()
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) != 3: continue
    rows.setdefault(p[0], []).append(float(p[1]))
    fails.setdefault(p[0], []).append(float(p[2]))
if not rows:
    print("no results"); sys.exit(1)

print(f"{'arm':<12}{'n':>3}  {'mean':>12}  {'spread%':>8}  {'max fail%':>10}")
print("-" * 52)
means = {}
for name, vals in rows.items():
    mean = statistics.fmean(vals)
    spread = 100 * (max(vals) - min(vals)) / mean if mean else 0
    means[name] = mean
    print(f"{name:<12}{len(vals):>3}  {mean:>12.3f}  {spread:>7.2f}%  "
          f"{max(fails[name]):>9.3f}%")

names = list(means)
base = names[0]
print()
worst_spread = max(
    100 * (max(v) - min(v)) / statistics.fmean(v) for v in rows.values())
for other in names[1:]:
    delta = 100 * (means[other] - means[base]) / means[base]
    # The gate: a change is only worth keeping if it clears the noise floor by
    # a comfortable margin. Ranges that overlap at all are not a result.
    overlap = not (min(rows[other]) > max(rows[base]) or
                   max(rows[other]) < min(rows[base]))
    verdict = "INCONCLUSIVE (ranges overlap)" if overlap else (
        "WIN"  if delta >= 2.0 else
        "LOSS" if delta <= -2.0 else
        "no meaningful difference (<2%)")
    print(f"{other} vs {base}: {delta:+.2f}%   {verdict}")
nmin = min(len(v) for v in rows.values())
if nmin < 2:
    print("\nonly one rep per arm: run-to-run spread is UNMEASURED, so no "
          "delta above is trustworthy. Use --reps 3 or more.")
else:
    print(f"\nworst per-arm spread {worst_spread:.2f}% "
          f"-- a delta smaller than this is not a result")
PY
rm -f "$RESULTS_FILE"
