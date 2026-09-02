#!/bin/bash
set -e

cd "$(dirname "$0")/build"

echo "Building..."
cmake --build . --target master_server worker_server

echo "Starting master..."
./master_server &
MASTER_PID=$!
sleep 1

echo "Starting worker1..."
./worker_server worker1 0.0.0.0:50052 localhost:50051 &
WORKER_PID=$!

echo "Master PID: $MASTER_PID, Worker PID: $WORKER_PID"
echo "Press Ctrl+C to stop both."

trap "kill $MASTER_PID $WORKER_PID 2>/dev/null" EXIT
wait
