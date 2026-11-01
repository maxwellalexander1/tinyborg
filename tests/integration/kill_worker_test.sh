#!/usr/bin/env bash
# T2: kill a worker mid-job, master must reschedule onto survivor.
# Uses fast timeout (3s) via env, defaults unchanged (10s).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
MASTER_ADDR="127.0.0.1:50151"
MASTER_BIND="0.0.0.0:50151"
JOB="killtest-$$"

MASTER_PID=""
W1_PID=""
W2_PID=""

cleanup() {
  kill "$MASTER_PID" "$W1_PID" "$W2_PID" 2>/dev/null || true
  pkill -f "/bin/sleep 60" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "starting master on $MASTER_BIND (timeout 3s)..."
TINYBORG_MASTER_ADDR="$MASTER_BIND" TINYBORG_DEAD_TIMEOUT_SEC=3 \
  "$BUILD/master_server" > /tmp/tb_master_$$.log 2>&1 &
MASTER_PID=$!
sleep 2

echo "starting workers..."
"$BUILD/worker_server" worker1 0.0.0.0:50152 "$MASTER_ADDR" > /tmp/tb_w1_$$.log 2>&1 &
W1_PID=$!
"$BUILD/worker_server" worker2 0.0.0.0:50153 "$MASTER_ADDR" > /tmp/tb_w2_$$.log 2>&1 &
W2_PID=$!
sleep 4

export TINYBORG_MASTER_ADDR="$MASTER_ADDR"
echo "submitting $JOB..."
"$BUILD/tbctl" submit "$JOB" /bin/sleep 60

echo "waiting for job to run..."
OWNER=""
for i in $(seq 1 10); do
  OUT="$("$BUILD/tbctl" status "$JOB" 2>&1 || true)"
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
  VICTIM_PID="$W1_PID"
  SURVIVOR="worker2"
  kill -9 "$W1_PID"
  W1_PID=""
else
  VICTIM_PID="$W2_PID"
  SURVIVOR="worker1"
  kill -9 "$W2_PID"
  W2_PID=""
fi

echo "killed $OWNER (pid $VICTIM_PID), waiting for reschedule to $SURVIVOR..."
for i in $(seq 1 15); do
  sleep 1
  OUT="$("$BUILD/tbctl" status "$JOB" 2>&1 || true)"
  echo "poll $i: $(echo "$OUT" | tr '\n' ' ')"
  if echo "$OUT" | grep -q "phase: RUNNING" && echo "$OUT" | grep -q "worker: $SURVIVOR"; then
    echo "PASS: job rescheduled to $SURVIVOR"
    exit 0
  fi
done

echo "FAIL: job did not reschedule to $SURVIVOR"
exit 1
