#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"

if [ "${1:-}" = "--insecure" ]; then
  echo "WARNING: running with --insecure, no TLS, dev only"
  export TINYBORG_INSECURE=1
else
  if [ ! -f "$ROOT/certs/ca.crt" ]; then
    echo "Generating certs..."
    bash "$ROOT/scripts/gen_certs.sh" "$ROOT/certs"
  fi
  if [ ! -f "$ROOT/certs/submit/submitter.key" ]; then
    bash "$ROOT/scripts/gen_submit_keys.sh" "$ROOT/certs/submit"
  fi
  unset TINYBORG_INSECURE
  export TINYBORG_CA_CRT="$ROOT/certs/ca.crt"
  export TINYBORG_SUBMIT_KEY="$ROOT/certs/submit/submitter.key"
  export TINYBORG_SUBMIT_KEY_ID="default"
  export TINYBORG_TRUSTED_PUBKEY="$ROOT/certs/submit/submitter.pub"
  export TINYBORG_TRUSTED_KEY_ID="default"
fi

cd "$ROOT/build"

echo "Building..."
cmake --build . --target master_server worker_server tbctl

echo "Starting master..."
TINYBORG_SERVER_CRT="$ROOT/certs/master.crt" TINYBORG_SERVER_KEY="$ROOT/certs/master.key" \
TINYBORG_CLIENT_CRT="$ROOT/certs/master.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/master.key" \
  ./master_server &
MASTER_PID=$!
sleep 1

echo "Starting worker1..."
TINYBORG_SERVER_CRT="$ROOT/certs/worker1.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker1.key" \
TINYBORG_CLIENT_CRT="$ROOT/certs/worker1.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker1.key" \
  ./worker_server worker1 127.0.0.1:50052 localhost:50051 &
WORKER1_PID=$!

echo "Starting worker2..."
TINYBORG_SERVER_CRT="$ROOT/certs/worker2.crt" TINYBORG_SERVER_KEY="$ROOT/certs/worker2.key" \
TINYBORG_CLIENT_CRT="$ROOT/certs/worker2.crt" TINYBORG_CLIENT_KEY="$ROOT/certs/worker2.key" \
  ./worker_server worker2 127.0.0.1:50053 localhost:50051 &
WORKER2_PID=$!

echo "Master PID: $MASTER_PID, Worker1 PID: $WORKER1_PID, Worker2 PID: $WORKER2_PID"
echo "Press Ctrl+C to stop all."

trap "kill $MASTER_PID $WORKER1_PID $WORKER2_PID 2>/dev/null" EXIT
wait
