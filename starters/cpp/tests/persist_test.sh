#!/bin/sh
# Restart-survival test for the persistence bonus.
#
# Simulates the stricter grading case: `docker rm` + fresh `docker run` (new
# container filesystem), with only a named volume carrying the data across.
# Passes iff a POSTed price survives into the second container.
#
# Usage: ./tests/persist_test.sh  (from starters/cpp, needs docker)
set -eu

IMG=obsidio-cpp-persist-test
CTR=obsidio-persist-test
VOL=obsidio-persist-test-data
PORT=8099

cleanup() {
  docker rm -f "$CTR" >/dev/null 2>&1 || true
  docker volume rm "$VOL" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

docker build -q -t "$IMG" . >/dev/null

run_ctr() {
  docker run -d --name "$CTR" --cpus=2 --memory=2g \
    -v "$VOL":/data -p "$PORT":8080 "$IMG" >/dev/null
  for _ in $(seq 1 50); do
    curl -4 -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return 0
    sleep 0.2
  done
  echo "FAIL: server never became healthy"; exit 1
}

run_ctr
curl -4 -sf -X POST "http://127.0.0.1:$PORT/price" \
  -H 'Content-Type: application/json' \
  -d '{"symbol":"AAPL","price":424.42}' >/dev/null

# Kill without graceful shutdown: persistence must not depend on a clean exit.
docker kill "$CTR" >/dev/null
docker rm "$CTR" >/dev/null

run_ctr
GOT=$(curl -4 -sf "http://127.0.0.1:$PORT/price?symbol=AAPL")
case "$GOT" in
  *424.42*) echo "PASS: price survived rm + fresh run ($GOT)" ;;
  *) echo "FAIL: expected 424.42, got: $GOT"; exit 1 ;;
esac
