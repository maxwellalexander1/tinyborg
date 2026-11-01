#!/usr/bin/env bash
# T8 negative tests: unauthenticated and wrong-role clients are rejected.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
M_PORT="50251"
M_ADDR="127.0.0.1:$M_PORT"

if [ ! -f "$ROOT/certs/ca.crt" ]; then
  bash "$ROOT/scripts/gen_certs.sh" "$ROOT/certs"
fi
CA="$ROOT/certs/ca.crt"

cleanup() {
  kill "$M_PID" "$W_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "starting master..."
TINYBORG_MASTER_ADDR="0.0.0.0:$M_PORT" TINYBORG_DEAD_TIMEOUT_SEC=10 \
  TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/master.crt" TINYBORG_SERVER_KEY="$ROOT/certs/master.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/master.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/master.key" \
  "$BUILD/master_server" > /tmp/tb_mtls_master_$$.log 2>&1 &
M_PID=$!
sleep 2

echo "starting worker1..."
TINYBORG_CA_CRT="$CA" \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker1.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker1.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  "$BUILD/worker_server" worker1 "127.0.0.1:50252" "$M_ADDR" > /tmp/tb_mtls_w_$$.log 2>&1 &
W_PID=$!
sleep 3

export TINYBORG_MASTER_ADDR="$M_ADDR"
export TINYBORG_CA_CRT="$CA"

fail=0

echo "--- test1: no cert (insecure client) must fail ---"
if TINYBORG_INSECURE=1 "$BUILD/tbctl" --insecure submit evil1 /bin/sleep 5 2>&1; then
  echo "FAIL: insecure client was accepted"
  fail=1
else
  echo "PASS: insecure client rejected"
fi

echo "--- test2: wrong CA cert must fail ---"
if TINYBORG_CLIENT_CRT="$ROOT/certs/evil/intruder.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/evil/intruder.key" \
  "$BUILD/tbctl" submit evil2 /bin/sleep 5 2>&1; then
  echo "FAIL: wrong-CA client was accepted"
  fail=1
else
  echo "PASS: wrong-CA client rejected"
fi

echo "--- test3: worker cert cannot submit (role check) ---"
if TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  "$BUILD/tbctl" submit evil3 /bin/sleep 5 2>&1 | grep -qi "denied\|permission"; then
  echo "PASS: worker cert blocked from submit"
else
  echo "checking raw output for denial..."
  if TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
    "$BUILD/tbctl" submit evil3 /bin/sleep 5 2>&1; then
    echo "FAIL: worker cert submit accepted"
    fail=1
  else
    echo "PASS: worker cert submit failed (see above)"
  fi
fi

echo "--- test4: valid tbctl-client submit works ---"
export TINYBORG_CLIENT_CRT="$ROOT/certs/tbctl-client.crt"
export TINYBORG_CLIENT_KEY="$ROOT/certs/tbctl-client.key"
if "$BUILD/tbctl" submit good1 /bin/sleep 5 2>&1 | grep -q "OK"; then
  echo "PASS: valid client accepted"
else
  echo "FAIL: valid client rejected"
  fail=1
fi

exit $fail
