#!/usr/bin/env bash
# T9: workers refuse unsigned, tampered, expired, replayed manifests.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$ROOT/build"
M_ADDR="127.0.0.1:50451"
M_BIND="0.0.0.0:50451"

if [ ! -f "$ROOT/certs/ca.crt" ]; then bash "$ROOT/scripts/gen_certs.sh" "$ROOT/certs"; fi
if [ ! -f "$ROOT/certs/submit/submitter.key" ]; then bash "$ROOT/scripts/gen_submit_keys.sh" "$ROOT/certs/submit"; fi
CA="$ROOT/certs/ca.crt"
export TINYBORG_MASTER_ADDR="$M_ADDR"
export TINYBORG_SUBMIT_KEY="$ROOT/certs/submit/submitter.key"
export TINYBORG_SUBMIT_KEY_ID="default"
export TINYBORG_TRUSTED_PUBKEY="$ROOT/certs/submit/submitter.pub"
export TINYBORG_TRUSTED_KEY_ID="default"

M_PID=""; W_PID=""
cleanup() {
  kill "$M_PID" "$W_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

echo "starting master + worker..."
TINYBORG_MASTER_ADDR="$M_BIND" TINYBORG_DEAD_TIMEOUT_SEC=10 TINYBORG_LEASE_SEC=5 \
  TINYBORG_CA_CRT="$CA" TINYBORG_SERVER_CRT="$ROOT/certs/master.crt" TINYBORG_SERVER_KEY="$ROOT/certs/master.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/master.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/master.key" \
  "$BUILD/master_server" > /tmp/tb_man_master_$$.log 2>&1 &
M_PID=$!
sleep 2
TINYBORG_CA_CRT="$CA" TINYBORG_TRUSTED_PUBKEY="$ROOT/certs/submit/submitter.pub" \
  TINYBORG_SERVER_CRT="$ROOT/certs/worker1.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker1.key" \
  TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  "$BUILD/worker_server" worker1 127.0.0.1:50452 "$M_ADDR" > /tmp/tb_man_w_$$.log 2>&1 &
W_PID=$!
sleep 3

run_tbctl() {
  TINYBORG_CA_CRT="$CA" TINYBORG_MASTER_ADDR="$M_ADDR" \
    TINYBORG_CLIENT_CRT="$ROOT/certs/tbctl-client.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/tbctl-client.key" \
    "$BUILD/tbctl" "$@"
}

fail=0
check_never_runs() {
  local job="$1" desc="$2"
  for i in $(seq 1 5); do
    OUT="$(run_tbctl status "$job" 2>&1 || true)"
    if echo "$OUT" | grep -q "phase: RUNNING"; then
      echo "FAIL: $desc ran but should have been refused"
      return 1
    fi
    sleep 1
  done
  echo "PASS: $desc never ran"
  return 0
}

echo "--- positive: valid signed job runs ---"
run_tbctl submit "man-good-$$" /bin/sleep 5
for i in $(seq 1 10); do
  OUT="$(run_tbctl status "man-good-$$" 2>&1 || true)"
  echo "$OUT" | tr '\n' ' '; echo
  echo "$OUT" | grep -q "phase: RUNNING" && break
  sleep 1
done
run_tbctl status "man-good-$$" | grep -q "phase: RUNNING" || { echo "FAIL: valid job did not run"; fail=1; }

echo "--- expired manifest refused ---"
TINYBORG_MANIFEST_TTL_SEC=-10 run_tbctl submit "man-exp-$$" /bin/sleep 5
check_never_runs "man-exp-$$" "expired manifest" || fail=1

echo "--- wrong-key signature refused ---"
openssl genpkey -algorithm Ed25519 -out /tmp/evil_sub.key 2>/dev/null
TINYBORG_SUBMIT_KEY=/tmp/evil_sub.key run_tbctl submit "man-evil-$$" /bin/sleep 5
check_never_runs "man-evil-$$" "wrong-key manifest" || fail=1
rm -f /tmp/evil_sub.key

echo "--- unsigned manifest refused ---"
( unset TINYBORG_SUBMIT_KEY; run_tbctl submit "man-unsigned-$$" /bin/sleep 5 ) || true
check_never_runs "man-unsigned-$$" "unsigned manifest" || fail=1

exit $fail
