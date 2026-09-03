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
WORKER1_PID=$!

echo "Starting worker2..."
./worker_server worker2 0.0.0.0:50053 localhost:50051 &
WORKER2_PID=$!

echo "Master PID: $MASTER_PID, Worker1 PID: $WORKER1_PID, Worker2 PID: $WORKER2_PID"
echo "Press Ctrl+C to stop all."

trap "kill $MASTER_PID $WORKER1_PID $WORKER2_PID 2>/dev/null" EXIT
wait
