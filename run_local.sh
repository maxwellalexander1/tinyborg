#!/bin/bash
set -e

cd "$(dirname "$0")"

if [ "${1:-}" = "--insecure" ]; then
  echo "WARNING: running with --insecure, no TLS, dev only"
  export TINYBORG_INSECURE=1
else
  if [ ! -f certs/ca.crt ]; then
    echo "Generating certs..."
    bash scripts/gen_certs.sh certs
  fi
  unset TINYBORG_INSECURE
fi

cd build

echo "Building..."
cmake --build . --target master_server worker_server tbctl

echo "Starting master..."
./master_server &
MASTER_PID=$!
sleep 1

echo "Starting worker1..."
./worker_server worker1 127.0.0.1:50052 localhost:50051 &
WORKER1_PID=$!

echo "Starting worker2..."
./worker_server worker2 127.0.0.1:50053 localhost:50051 &
WORKER2_PID=$!

echo "Master PID: $MASTER_PID, Worker1 PID: $WORKER1_PID, Worker2 PID: $WORKER2_PID"
echo "Press Ctrl+C to stop all."

trap "kill $MASTER_PID $WORKER1_PID $WORKER2_PID 2>/dev/null" EXIT
wait
