#!/usr/bin/env python3
"""TinyBorg benchmark: throughput, latency, workload size, concurrency model.

Usage:
  python3 bench.py                     # full run (auto-detects cluster, builds if needed)
  python3 bench.py --model-only        # just print exact concurrency/process model, no cluster needed
  python3 bench.py --jobs 50 --concurrency 16 --binary /bin/true
  python3 bench.py --jobs 20 --binary /bin/sleep --args 2 --workers 2

What it measures:
  1. Peak SubmitJob req/sec  (scheduler intake, concurrent SubmitJob RPCs via tbctl)
  2. End-to-end throughput    (jobs COMPLETED/sec, submit -> COMPLETED via status polling)
  3. Latency: avg/p50/p99 submit RPC, scheduling delay (submit->RUNNING), e2e (submit->COMPLETED)
  4. Latency improvement / speedup vs serial baseline (N * single-job time / makespan)
  5. Workload/data size + exact concurrency/process model scraped from source.

Concurrency model is NOT guessed -- values are extracted from the C++ sources.
"""
import argparse, concurrent.futures, os, re, shutil, socket, subprocess, sys, time, statistics

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
TBCTL = os.path.join(BUILD, "tbctl")
MASTER_ADDR = "localhost:50051"

def grep(path, pattern, default="?"):
    try:
        with open(os.path.join(ROOT, path)) as f:
            src = f.read()
        m = re.search(pattern, src, re.M)
        return m.group(1) if m else default
    except FileNotFoundError:
        return default

def get_model():
    """Scrape exact concurrency/process model from source."""
    tick = grep("master/main.cpp", r"sleep_for\(std::chrono::seconds\((\d+)\)\)")
    dead = grep("master/main.cpp", r"kDeadWorkerTimeout\((\d+)\)")
    hb = grep("worker/main.cpp", r"sleep_for\(std::chrono::seconds\((\d+)\)\)")
    cpu = grep("worker/main.cpp", r"kCpuCores\s*=\s*(\d+)")
    mem = grep("worker/main.cpp", r"kMemMb\s*=\s*(\d+)")
    req_cpu = grep("cli/tbctl.cpp", r"set_cpu_request\((\d+)\)")
    req_mem = grep("cli/tbctl.cpp", r"set_mem_request_mb\((\d+)\)")
    return {
        "sched_tick_s": tick, "dead_timeout_s": dead, "heartbeat_s": hb,
        "worker_cpu": cpu, "worker_mem_mb": mem,
        "job_cpu": req_cpu, "job_mem_mb": req_mem,
    }

def print_model(m, n_workers=2):
    wcpu, jcpu = int(m["worker_cpu"]), int(m["job_cpu"])
    max_per_worker = wcpu // max(jcpu, 1)
    total_slots = max_per_worker * n_workers
    print("== Exact concurrency / process model (from source) ==")
    print(f"  master/master/main.cpp SchedulingLoop : 1 thread, 1-sec tick (tick={m['sched_tick_s']}s), gRPC threadpool + mutex ClusterState")
    print(f"  failure detection                     : timeout={m['dead_timeout_s']}s missed heartbeats -> reschedule JobsOnWorker()")
    print(f"  scheduler                             : first-fit bin-pack, local reserve on AssignJobToWorker, preemption by priority")
    print(f"  worker/worker/main.cpp                : 1 gRPC server + ReapLoop thread (1s waitpid WNOHANG) + heartbeat thread (2s)")
    print(f"  job execution                         : fork()/execvp() real OS process per job, GetJobStatus proxies master->worker")
    print(f"  capacity                              : worker={wcpu} CPU/{m['worker_mem_mb']}MB, job req={jcpu} CPU/{m['job_mem_mb']}MB")
    print(f"  max concurrent jobs                   : {max_per_worker}/worker x {n_workers} workers = {total_slots} slots")
    print(f"  theoretical sched throughput cap      : ~{total_slots} jobs per {m['sched_tick_s']}s tick = ~{total_slots/int(m['sched_tick_s']):.0f} jobs/s completion (capacity-bound)")
    print()
    return total_slots

def tbctl(*args, timeout=15):
    t0 = time.perf_counter()
    p = subprocess.run([TBCTL, *args], capture_output=True, text=True, timeout=timeout)
    dt = time.perf_counter() - t0
    return p.returncode == 0 and "OK" in (p.stdout + p.stderr) or "phase" in (p.stdout + p.stderr), dt, (p.stdout.strip() + " " + p.stderr.strip()).strip()

def master_up():
    s = socket.socket(); s.settimeout(1)
    try:
        s.connect(("127.0.0.1", 50051)); s.close(); return True
    except OSError:
        return False

def ensure_cluster(n_workers):
    if master_up() and os.path.exists(TBCTL):
        return []
    procs = []
    if not os.path.exists(TBCTL):
        print(f"[build] cmake+make in {BUILD} ...")
        os.makedirs(BUILD, exist_ok=True)
        subprocess.run(["cmake", ".."], cwd=BUILD, check=False)
        subprocess.run(["cmake", "--build", ".", "--target", "master_server", "worker_server", "tbctl", "-j"], cwd=BUILD, check=False)
    if not os.path.exists(TBCTL):
        print(f"[warn] {TBCTL} missing after build; run with --model-only for static numbers."); sys.exit(1)
    if not master_up():
        print("[cluster] starting master + workers ...")
        procs.append(subprocess.Popen([os.path.join(BUILD, "master_server")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        for i in range(1, n_workers + 1):
            port = 50051 + i
            procs.append(subprocess.Popen([os.path.join(BUILD, "worker_server"), f"worker{i}", f"0.0.0.0:{port}", MASTER_ADDR],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(3)
    return procs

def poll_until_done(job_id, timeout=120):
    t0 = time.perf_counter()
    first_running = None
    while time.perf_counter() - t0 < timeout:
        ok, _, out = tbctl("status", job_id)
        if "COMPLETED" in out or "FAILED" in out:
            if first_running is None: first_running = time.perf_counter()
            return time.perf_counter() - t0, (first_running - t0) if first_running else 0, out
        if ("RUNNING" in out or "PENDING" in out) and first_running is None and "RUNNING" in out:
            first_running = time.perf_counter()
        time.sleep(0.5)
    return timeout, 0, "TIMEOUT"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=20, help="total jobs in batch")
    ap.add_argument("--concurrency", type=int, default=16, help="submit concurrency (threads)")
    ap.add_argument("--binary", default="/bin/true", help="job binary (/bin/true, /bin/sleep, ./pi_est)")
    ap.add_argument("--args", nargs="*", default=[], help="args for job binary, e.g. --args 2")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--sweep", default="1,4,8,16,32", help="concurrency sweep for req/sec test")
    ap.add_argument("--model-only", action="store_true")
    a = ap.parse_args()

    m = get_model()
    slots = print_model(m, a.workers)
    workload = f"{a.jobs} jobs x `{a.binary} {' '.join(a.args)}`"
    print(f"== Workload / data size ==\n  batch: {workload}\n  sweep concurrencies: {a.sweep}\n")
    if a.model_only:
        print("[model-only] no live cluster run. Start one with: (cd build && ./master_server &) + workers, then re-run without --model-only.")
        return

    procs = ensure_cluster(a.workers)
    try:
        prefix = f"b{int(time.time()) % 100000}"
        # --- single-job baseline (serial latency reference) ---
        print("--- baseline: 1 job end-to-end ---")
        ok, sub_dt, _ = tbctl("submit", f"{prefix}-base", a.binary, *a.args)
        e2e, _, _ = poll_until_done(f"{prefix}-base")
        print(f"  submit RPC: {sub_dt*1000:.1f} ms | e2e (submit->COMPLETED): {e2e:.2f}s")
        serial_est = e2e * a.jobs
        print(f"  serial baseline for {a.jobs} jobs ~= {serial_est:.1f}s (N x single e2e)")

        # --- sweep: peak SubmitJob req/sec at various concurrencies ---
        print("\n--- peak SubmitJob req/sec (intake) ---")
        best = (0, 0)
        for c in [int(x) for x in a.sweep.split(",")]:
            ids = [f"{prefix}-s{c}-{i}" for i in range(a.jobs)]
            t0 = time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=c) as ex:
                futs = {ex.submit(tbctl, "submit", j, a.binary, *a.args): j for j in ids}
                lats = [f.result()[1] for f in concurrent.futures.as_completed(futs)]
            wall = time.perf_counter() - t0
            rps = a.jobs / wall
            best = max(best, (rps, c))
            print(f"  conc={c:3d}  {a.jobs:4d} submits in {wall:6.2f}s  -> {rps:7.1f} req/s  submit p50={statistics.median(lats)*1000:.0f}ms p99={sorted(lats)[int(len(lats)*0.99)-1]*1000:.0f}ms")
            time.sleep(2)  # let scheduler drain before next sweep round
        print(f"  PEAK intake: {best[0]:.1f} req/s @ concurrency={best[1]}")

        # --- end-to-end batch: throughput + latency improvement ---
        print(f"\n--- end-to-end batch: {a.jobs} jobs @ submit-conc={a.concurrency} ---")
        ids = [f"{prefix}-e{i}" for i in range(a.jobs)]
        t0 = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.concurrency) as ex:
            list(ex.map(lambda j: tbctl("submit", j, a.binary, *a.args), ids))
        submit_wall = time.perf_counter() - t0
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(a.concurrency, 16)) as ex:
            res = list(ex.map(poll_until_done, ids))
        makespan = time.perf_counter() - t0
        e2es = [r[0] for r in res]
        done = sum(1 for r in res if "TIMEOUT" not in r[2])
        thr = done / makespan
        speedup = serial_est / makespan if makespan > 0 else 0
        print(f"  workload        : {workload} (x{a.jobs})")
        print(f"  submit phase    : {submit_wall:.2f}s ({a.jobs/submit_wall:.1f} req/s)")
        print(f"  makespan        : {makespan:.2f}s  completed {done}/{a.jobs}")
        print(f"  THROUGHPUT      : {thr:.2f} jobs/s completed end-to-end")
        print(f"  e2e latency/job : avg={statistics.mean(e2es):.2f}s p50={statistics.median(e2es):.2f}s p99={sorted(e2es)[int(len(e2es)*0.99)-1]:.2f}s")
        print(f"  LATENCY IMPROVEMENT vs serial: {serial_est:.1f}s -> {makespan:.2f}s = {speedup:.1f}x speedup ({100*(1-makespan/serial_est):.0f}% wall-time reduction)")
        print(f"\nRESULT peak_intake={best[0]:.1f}req/s(@c={best[1]}) throughput={thr:.2f}jobs/s makespan={makespan:.2f}s speedup={speedup:.1f}x workload=\"{workload}\"")
    finally:
        for p in procs:
            p.terminate()

if __name__ == "__main__":
    main()
