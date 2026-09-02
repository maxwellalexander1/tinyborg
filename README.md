# TinyBorg

A miniature version of Google's internal cluster scheduler (Borg), built to understand how large-scale infrastructure orchestration actually works under the hood.

## What it does

TinyBorg is a distributed job scheduler with a master/worker architecture:
- **Master**: accepts job submissions, tracks cluster state (workers + jobs), and schedules pending jobs onto workers with available capacity
- **Worker**: registers with the master, sends periodic heartbeats, and runs jobs as real OS processes (fork/exec) with lifecycle tracking (running, exited, killed)
- **CLI (`tbctl`)**: submit jobs and query their status from the command line

All communication is over gRPC, with protobuf-defined service contracts.

## Status

- [x] Master/worker registration + heartbeating
- [x] First-fit scheduling onto workers with capacity
- [x] Job lifecycle tracking (fork/exec, exit detection, kill handling)
- [x] CLI for submit/status
- [ ] Multi-worker load distribution (in progress)
- [ ] Failure detection + job rescheduling on worker death
- [ ] Resource enforcement via cgroups

## Stack

C++17, gRPC, Protocol Buffers, CMake

# Execution

TODO
