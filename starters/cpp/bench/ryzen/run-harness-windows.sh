#!/usr/bin/env bash
# Run the Ryzen measurement kit from a Windows host that has no WSL2 distro.
#
# ab.sh and the sweeps assume a Linux shell with docker and python3 on PATH.
# On this machine neither is available the easy way:
#
#   - No WSL2 distro exists, only the internal `docker-desktop` one, so
#     `wsl -e bash` has nothing to run.
#   - There is no host python3, only the Windows Store alias stub, and ab.sh
#     needs a real interpreter for both the effective-clock probe and the
#     summary table.
#
# So the harness runs in a container instead, and drives sibling containers
# through the mounted docker socket. Three details make that work, and all
# three were re-derived from scratch once already:
#
#   1. THE REPO IS MOUNTED AT THE PATH THE DAEMON RESOLVES, NOT A TIDY ONE.
#      ab.sh launches k6 with `-v "$HERE:/scripts:ro"`, where $HERE is a path
#      inside *this* container. That nested -v is interpreted by the host
#      daemon, not by us, so $HERE has to be a string the host daemon can turn
#      back into real host files. Mounting at /repo would hand the daemon
#      "/repo/..." and it would silently create an empty volume; k6 would then
#      run an empty script directory. Docker Desktop resolves the legacy
#      /c/Users/... form, so that is the mount point: same absolute path in and
#      out.
#
#   2. AN LF COPY OF THIS DIRECTORY IS WHAT ACTUALLY EXECUTES.
#      core.autocrlf is true on this checkout, so the working-copy scripts have
#      CRLF and bash rejects `set -uo pipefail\r`. .gitattributes now pins *.sh
#      to LF, which fixes it for anyone who re-checks-out, but this stays as
#      belt and braces: it costs a copy and it is the difference between a
#      confusing failure and none.
#
#   3. MSYS_NO_PATHCONV=1 ON EVERY docker run THAT CARRIES -v.
#      Git Bash rewrites anything that looks like a Unix path in an argument,
#      so /var/run/docker.sock becomes C:/Program Files/Git/var/run/docker.sock
#      and the mount fails. This script sets it for its own invocations; if you
#      run docker by hand from Git Bash you need it too.
#
# Usage -- arguments are passed straight through to ab.sh:
#
#   ./run-harness-windows.sh --reps 3 "sha-ni:" "fallback:RISK_BACKEND=reference"
#   RISK_PCT=0 ./run-harness-windows.sh --probe mixed --reps 3 "cheaponly:IO_THREADS=2"
#
# Or name a different script in this directory, or get a shell:
#
#   ./run-harness-windows.sh --script config_sweep.sh --reps 3
#   ./run-harness-windows.sh --shell
#
# The full grading script does NOT need any of this -- it has no nested mount
# and no python3 -- see README.md, "Running the grading script from Windows".
set -uo pipefail

SCRIPT="ab.sh"
WANT_SHELL=0
HARNESS_IMAGE="${HARNESS_IMAGE:-obsidio-harness}"

while [ $# -gt 0 ]; do
  case "$1" in
    --script) SCRIPT="$2"; shift 2 ;;
    --shell)  WANT_SHELL=1; shift ;;
    -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
    *) break ;;
  esac
done

command -v docker >/dev/null || { echo "error: docker not found on PATH" >&2; exit 2; }
docker version >/dev/null 2>&1 || {
  echo "error: docker is installed but the daemon is not answering." >&2
  echo "       Start Docker Desktop and wait for it to report Running." >&2
  exit 2
}

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
[ -d "$REPO/k6" ] || { echo "error: $REPO does not look like the repo root" >&2; exit 2; }

# C:/Users/YASH/achal-projects/obsidio -- what the -v source must say.
if command -v cygpath >/dev/null 2>&1; then
  HOST_PATH="$(cygpath -m "$REPO")"
else
  HOST_PATH="$(cd "$REPO" && pwd -W 2>/dev/null || pwd)"
fi

# /c/Users/YASH/achal-projects/obsidio -- what the -v target must say, because
# that string is what ends up in ab.sh's own nested -v. See note 1 above.
CONTAINER_PATH="$(printf '%s' "$HOST_PATH" \
  | sed -E 's#^([A-Za-z]):#/\L\1#' | tr '\\' '/')"

case "$CONTAINER_PATH" in
  /?/*) ;;
  *) echo "error: could not derive a daemon-resolvable path from '$HOST_PATH'." >&2
     echo "       Expected a Windows drive path. Run this from Git Bash." >&2
     exit 2 ;;
esac

RYZEN_REL="starters/cpp/bench/ryzen"
LF_DIR="$REPO/$RYZEN_REL/.lf"
LF_CONTAINER_DIR="$CONTAINER_PATH/$RYZEN_REL/.lf"

# Note 2: strip CR and re-mark executable. Regenerated every run, so an edit to
# ab.sh never gets shadowed by a stale copy.
rm -rf "$LF_DIR" && mkdir -p "$LF_DIR" || exit 1
for f in "$REPO/$RYZEN_REL"/*.sh "$REPO/$RYZEN_REL"/*.js; do
  [ -e "$f" ] || continue
  base="$(basename "$f")"
  [ "$base" = "$(basename "$0")" ] && continue   # no point copying this one
  tr -d '\r' < "$f" > "$LF_DIR/$base" || exit 1
done
chmod +x "$LF_DIR"/*.sh 2>/dev/null

[ -f "$LF_DIR/$SCRIPT" ] || {
  echo "error: no such script in $RYZEN_REL: $SCRIPT" >&2
  exit 2
}

# The harness image: docker:cli plus the two things ab.sh needs that it lacks.
# Built once and reused; delete it with `docker rmi obsidio-harness` to refresh.
if ! docker image inspect "$HARNESS_IMAGE" >/dev/null 2>&1; then
  echo "building $HARNESS_IMAGE (once)..."
  docker build -t "$HARNESS_IMAGE" - <<'DOCKERFILE' || exit 1
FROM docker:cli
RUN apk add --no-cache bash python3
DOCKERFILE
fi

# Forward the knobs ab.sh reads from the environment. RISK_PCT is the one that
# matters most: it was silently dropped on the way to k6 until 69f4f91, and a
# run that quietly measures the wrong mix is worse than one that fails.
env_args=()
for v in RISK_PCT K6_CPUSET SUT_CPUS CLOCK_FLOOR_PCT IMAGE REPS DURATION VUS COOLDOWN; do
  eval "val=\${$v:-}"
  [ -n "$val" ] && env_args+=(-e "$v=$val")
done

echo "repo (host)      : $HOST_PATH"
echo "repo (daemon)    : $CONTAINER_PATH"
echo "running          : $SCRIPT $*"
echo

if [ "$WANT_SHELL" = "1" ]; then
  set -- bash
else
  set -- bash "./$SCRIPT" "$@"
fi

# --network host so the harness can reach the bridge network it creates, and
# -it only when there is a terminal, so this stays usable from CI or a pipe.
tty_args=()
[ -t 0 ] && [ -t 1 ] && tty_args=(-it)

MSYS_NO_PATHCONV=1 exec docker run --rm "${tty_args[@]}" \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -v "$HOST_PATH:$CONTAINER_PATH" \
  -w "$LF_CONTAINER_DIR" \
  "${env_args[@]}" \
  "$HARNESS_IMAGE" "$@"
