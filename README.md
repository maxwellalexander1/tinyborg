# TinyBorg

A miniature version of Google's internal cluster scheduler (Borg), built from scratch in C++ and gRPC to understand how large-scale infrastructure orchestration actually works.

## What it does

TinyBorg is a distributed job scheduler with a master/worker control plane:

- **Master**: accepts job submissions, tracks cluster state (workers + jobs), schedules pending jobs onto workers with available capacity, detects worker failures, and reschedules their work automatically
- **Worker**: registers with the master, sends periodic heartbeats reporting real resource usage, and runs jobs as real OS processes (fork/exec) with full lifecycle tracking
- **CLI (`tbctl`)**: submit jobs and query their status from the command line

All communication is over gRPC, with protobuf-defined service contracts.

## Proof it actually works: distributed Monte Carlo pi estimation

To validate this isn't just a scheduler that runs `sleep`, I distributed a real CPU-bound workload (Monte Carlo pi estimation, 640M random samples) across the cluster and compared it against running the same total work as a single job:

| Configuration | Samples | Time | Result |
|---|---|---|---|
| 1 job, 1 worker | 640,000,000 | 41s | pi ≈ 3.14164 |
| 8 jobs, 2 workers | 640,000,000 | 11s | pi ≈ 3.14169 |

**3.7x speedup with only 2 workers**, both configurations converge to the correct value of pi, confirming correctness held under real parallel execution, not just scheduling logic in isolation.

## Features

- **Resource-aware scheduling**: first-fit bin-packing based on live worker capacity, race-condition-free (capacity is reserved locally at scheduling time, reconciled by heartbeats, preventing overcommitment when multiple jobs are scheduled in the same tick)
- **Failure detection + automatic rescheduling**: master detects dead workers via missed heartbeats and reschedules their jobs onto live workers, with no manual intervention
- **Priority preemption**: high-priority jobs can evict the minimum necessary number of lower-priority jobs to make room, mirroring Borg's scheduling classes
- **Process lifecycle management**: fork/exec-based job execution with non-blocking child reaping, correctly distinguishing normal exit, signal termination, and forced kill
- **CLI**: submit and query job status against a running cluster

## Known limitations (things a real system would need)

- No fencing/leases: a network-partitioned worker that's still alive could theoretically keep running a job the master has already rescheduled elsewhere (the classic distributed systems "split brain" problem)
- Fixed heartbeat timeout rather than adaptive (TCP RTO-style) detection
- No resource enforcement (cgroups) at the OS level, jobs can exceed their declared limits
- No backoff on preemption, a low-priority job could theoretically be repeatedly preempted under sustained high-priority load

## Stack

C++17, gRPC, Protocol Buffers, CMake

## Why

Built this to get hands-on with the primitives that real infrastructure runs on: distributed state management, heartbeat-based failure detection, resource-aware scheduling, and process lifecycle control, the same fundamentals behind systems like Google's Borg and Kubernetes.
