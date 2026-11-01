#!/usr/bin/env bash
# T2+T8: kill a worker mid-job, master must reschedule onto survivor.
# mTLS on by default (certs/ via gen_certs.sh). --insecure for dev only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
MASTER_ADDR="127.0.0.1:50151"
MASTER_BIND="0.0.0.0:50151"
JOB="killtest-$$"

if [ "${1:-}" = "--insecure" ]; then
  export TINYBORG_INSECURE=1
  echo "WARNING: insecure mode"
else
  unset TINYBORG_INSECURE
  if [ ! -f "$ROOT/certs/ca.crt" ]; then
    bash "$ROOT/scripts/gen_certs.sh" "$ROOT/certs"
  fi
fi

CA="$ROOT/certs/ca.crt"
export TINYBORG_MASTER_ADDR="$MASTER_ADDR"

MASTER_PID=""
W1_PID=""
W2_PID=""

cleanup() {
  kill "$MASTER_PID" "$W1_PID" "$W2_PID" 2>/dev/null || true
  pkill -f "/bin/sleep 60" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "starting master on $MASTER_BIND (timeout 3s, mTLS)..."
TINYBORG_MASTER_ADDR="$MASTER_BIND" TINYBORG_DEAD_TIMEOUT_SEC=3 \
  TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/master.crt" TINYBORG_SERVER_KEY="$ROOT/certs/master.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/master.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/master.key" \
  "$BUILD/master_server" > /tmp/tb_master_$$.log 2>&1 &
MASTER_PID=$!
sleep 2

echo "starting workers..."
TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker1.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker1.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  "$BUILD/worker_server" worker1 127.0.0.1:50152 "$MASTER_ADDR" > /tmp/tb_w1_$$.log 2>&1 &
W1_PID=$!
TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker2.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker2.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker2.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker2.key" \
  "$BUILD/worker_server" worker2 127.0.0.1:50153 "$MASTER_ADDR" > /tmp/tb_w2_$$.log 2>&1 &
W2_PID=$!
sleep 4

run_tbctl() {
  TINYBORG_CA_CRT="$CA" \
    TINYBORG_CLIENT_CRT="$ROOT/certs/tbctl-client.crt" \
    TINYBORG_CLIENT_KEY="$ROOT/certs/tbctl-client.key" \
    "$BUILD/tbctl" "$@"
}

echo "submitting $JOB..."
run_tbctl submit "$JOB" /bin/sleep 60

echo "waiting for job to run..."
OWNER=""
for i in $(seq 1 10); do
  OUT="$(run_tbctl status "$JOB" 2>&1 || true)"
  echo "$OUT"
  if echo "$OUT" | grep -q "phase: RUNNING"; then
    OWNER="$(echo "$OUT" | grep '^worker:' | awk '{print $2}')"
    if [ -n "$OWNER" ]; then
      break
    fi
  fi
  sleep 1
done

if [ -z "$OWNER" ]; then
  echo "FAIL: job never reached RUNNING with a worker"
  exit 1
fi
echo "job is on $OWNER, killing it..."

if [ "$OWNER" = "worker1" ]; then
  SURVIVOR="worker2"
  kill -9 "$W1_PID"
  W1_PID=""
else
  SURVIVOR="worker1"
  kill -9 "$W2_PID"
  W2_PID=""
fi

echo "killed $OWNER, waiting for reschedule to $SURVIVOR..."
for i in $(seq 1 15); do
  sleep 1
  OUT="$(run_tbctl status "$JOB" 2>&1 || true)"
  echo "poll $i: $(echo "$OUT" | tr '\n' ' ')"
  if echo "$OUT" | grep -q "phase: RUNNING" && echo "$OUT" | grep -q "worker: $SURVIVOR"; then
    echo "PASS: job rescheduled to $SURVIVOR (mTLS)"
    exit 0
  fi
done

echo "FAIL: job did not reschedule to $SURVIVOR"
exit 1
