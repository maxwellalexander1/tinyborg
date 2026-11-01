#!/usr/bin/env bash
# T13: partition a worker with SIGSTOP, master reschedules, fencing rejects stale.
# Lease (3s) < dead timeout (6s) so zombie kills itself first.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
M_ADDR="127.0.0.1:50351"
M_BIND="0.0.0.0:50351"
JOB="fence-$$"

if [ ! -f "$ROOT/certs/ca.crt" ]; then
  bash "$ROOT/scripts/gen_certs.sh" "$ROOT/certs"
fi
CA="$ROOT/certs/ca.crt"
if [ ! -f "$ROOT/certs/submit/submitter.key" ]; then
  bash "$ROOT/scripts/gen_submit_keys.sh" "$ROOT/certs/submit"
fi
export TINYBORG_SUBMIT_KEY="$ROOT/certs/submit/submitter.key"
export TINYBORG_SUBMIT_KEY_ID="default"
export TINYBORG_TRUSTED_PUBKEY="$ROOT/certs/submit/submitter.pub"
export TINYBORG_TRUSTED_KEY_ID="default"


M_PID=""; W1_PID=""; W2_PID=""
cleanup() {
  kill "$M_PID" "$W1_PID" "$W2_PID" 2>/dev/null || true
  pkill -f "/bin/sleep 60" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "starting master (dead 6s)..."
TINYBORG_MASTER_ADDR="$M_BIND" TINYBORG_DEAD_TIMEOUT_SEC=6 TINYBORG_LEASE_SEC=3 \
  TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/master.crt" TINYBORG_SERVER_KEY="$ROOT/certs/master.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/master.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/master.key" \
  "$BUILD/master_server" > /tmp/tb_fence_master_$$.log 2>&1 &
M_PID=$!
sleep 2

echo "starting workers (lease 3s)..."
TINYBORG_CA_CRT="$CA" TINYBORG_LEASE_SEC=3 \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker1.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker1.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  "$BUILD/worker_server" worker1 127.0.0.1:50352 "$M_ADDR" > /tmp/tb_fence_w1_$$.log 2>&1 &
W1_PID=$!
TINYBORG_CA_CRT="$CA" TINYBORG_LEASE_SEC=3 \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker2.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker2.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker2.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker2.key" \
  "$BUILD/worker_server" worker2 127.0.0.1:50353 "$M_ADDR" > /tmp/tb_fence_w2_$$.log 2>&1 &
W2_PID=$!
sleep 4

run_tbctl() {
  TINYBORG_CA_CRT="$CA" TINYBORG_MASTER_ADDR="$M_ADDR" \
    TINYBORG_CLIENT_CRT="$ROOT/certs/tbctl-client.crt" \
    TINYBORG_CLIENT_KEY="$ROOT/certs/tbctl-client.key" \
    "$BUILD/tbctl" "$@"
}

run_tbctl submit "$JOB" /bin/sleep 60
OWNER=""
for i in $(seq 1 10); do
  OUT="$(run_tbctl status "$JOB" 2>&1 || true)"
  if echo "$OUT" | grep -q "phase: RUNNING"; then
    OWNER="$(echo "$OUT" | grep '^worker:' | awk '{print $2}')"
    [ -n "$OWNER" ] && break
  fi
  sleep 1
done
[ -z "$OWNER" ] && { echo "FAIL: never RUNNING"; exit 1; }
GEN1="$(run_tbctl status "$JOB" | grep '^generation:' | awk '{print $2}')"
echo "owner=$OWNER gen=$GEN1, stopping $OWNER (SIGSTOP = partition)..."
if [ "$OWNER" = "worker1" ]; then STOP_PID="$W1_PID"; SURVIVOR="worker2"; else STOP_PID="$W2_PID"; SURVIVOR="worker1"; fi
kill -STOP "$STOP_PID"

echo "waiting for reschedule to $SURVIVOR..."
OK=0
for i in $(seq 1 20); do
  sleep 1
  OUT="$(run_tbctl status "$JOB" 2>&1 || true)"
  echo "poll $i: $(echo "$OUT" | tr '\n' ' ')"
  if echo "$OUT" | grep -q "phase: RUNNING" && echo "$OUT" | grep -q "worker: $SURVIVOR"; then
    OK=1; break
  fi
done
[ "$OK" = "0" ] && { echo "FAIL: no reschedule"; exit 1; }
GEN2="$(run_tbctl status "$JOB" | grep '^generation:' | awk '{print $2}')"
echo "rescheduled gen=$GEN2"
if [ "$GEN2" -le "$GEN1" ]; then echo "FAIL: generation did not increase ($GEN1 -> $GEN2)"; exit 1; fi

echo "resuming old owner, its copy must be lease-killed and ignored..."
kill -CONT "$STOP_PID"
sleep 6
FINAL="$(run_tbctl status "$JOB" 2>&1 || true)"
echo "final: $(echo "$FINAL" | tr '\n' ' ')"
if ! echo "$FINAL" | grep -q "worker: $SURVIVOR"; then echo "FAIL: result not from survivor"; exit 1; fi
if ! echo "$FINAL" | grep -q "phase: RUNNING"; then echo "FAIL: not RUNNING"; exit 1; fi
echo "PASS: fencing holds (gen $GEN1 -> $GEN2, survivor $SURVIVOR)"
