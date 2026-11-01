#include <gtest/gtest.h>
#include <thread>
#include "scheduler.h"
#include "cluster_state.h"

using namespace tinyborg;
using namespace std::chrono_literals;

static WorkerRecord MakeWorker(const std::string& id, int32_t cpu_avail, int64_t mem_avail) {
  WorkerRecord w;
  w.worker_id = id;
  w.address = "localhost:0";
  w.cpu_cores = 4;
  w.mem_mb = 8192;
  w.cpu_available = cpu_avail;
  w.mem_available_mb = mem_avail;
  w.alive = true;
  w.last_heartbeat = std::chrono::steady_clock::now();
  return w;
}

static JobRecord MakeJob(const std::string& id, int32_t cpu, int64_t mem, int32_t prio = 0) {
  JobRecord j;
  j.job_id = id;
  j.binary_path = "/bin/sleep";
  j.args = {"10"};
  j.cpu_request = cpu;
  j.mem_request_mb = mem;
  j.priority = prio;
  j.state = JobDesiredState::PENDING;
  return j;
}

TEST(Scheduler, FirstFitExactFit) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 1, 256));
  Scheduler s;
  auto job = MakeJob("j1", 1, 256);
  auto d = s.PickWorker(job, state);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->worker_id, "w1");
  EXPECT_TRUE(d->jobs_to_preempt.empty());
}

TEST(Scheduler, NoFitReturnsNullopt) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 1, 256));
  Scheduler s;
  auto job = MakeJob("big", 8, 16384);
  EXPECT_FALSE(s.PickWorker(job, state).has_value());
}

TEST(Scheduler, SkipsFullWorkerForEmptyOne) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("full", 0, 0));
  state.RegisterWorker(MakeWorker("free", 4, 8192));
  Scheduler s;
  auto d = s.PickWorker(MakeJob("j", 1, 256), state);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->worker_id, "free");
}

TEST(Scheduler, ReservationPreventsDoubleBooking) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 2, 512));
  Scheduler s;
  auto j1 = MakeJob("j1", 2, 512);
  auto d1 = s.PickWorker(j1, state);
  ASSERT_TRUE(d1.has_value());
  state.AddJob(j1);
  state.AssignJobToWorker("j1", d1->worker_id);
  // Same worker now has 0 free, second identical job must not fit.
  auto j2 = MakeJob("j2", 2, 512);
  EXPECT_FALSE(s.PickWorker(j2, state).has_value());
}

TEST(Scheduler, HighPriorityPreemptsLowestFirst) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 0, 0));
  JobRecord low1 = MakeJob("low1", 1, 256, 1);
  low1.assigned_worker_id = "w1";
  low1.state = JobDesiredState::RUNNING;
  JobRecord low2 = MakeJob("low2", 1, 256, 5);
  low2.assigned_worker_id = "w1";
  low2.state = JobDesiredState::RUNNING;
  state.AddJob(low1);
  state.AddJob(low2);
  Scheduler s;
  auto hi = MakeJob("hi", 1, 256, 10);
  auto d = s.PickWorker(hi, state);
  ASSERT_TRUE(d.has_value());
  ASSERT_EQ(d->jobs_to_preempt.size(), 1u);
  EXPECT_EQ(d->jobs_to_preempt[0], "low1");
}

TEST(Scheduler, PreemptsMinimumNecessary) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 0, 0));
  for (int i = 0; i < 3; i++) {
    JobRecord low = MakeJob("low" + std::to_string(i), 1, 256, 1);
    low.assigned_worker_id = "w1";
    low.state = JobDesiredState::RUNNING;
    state.AddJob(low);
  }
  Scheduler s;
  auto hi = MakeJob("hi", 2, 512, 10);
  auto d = s.PickWorker(hi, state);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->jobs_to_preempt.size(), 2u);
}

TEST(Scheduler, NoPreemptionAgainstEqualOrHigherPriority) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 0, 0));
  JobRecord peer = MakeJob("peer", 2, 512, 5);
  peer.assigned_worker_id = "w1";
  peer.state = JobDesiredState::RUNNING;
  state.AddJob(peer);
  Scheduler s;
  EXPECT_FALSE(s.PickWorker(MakeJob("hi", 1, 256, 5), state).has_value());
  EXPECT_FALSE(s.PickWorker(MakeJob("lo", 1, 256, 1), state).has_value());
}

TEST(ClusterState, DeadWorkerDetectedAfterTimeout) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 4, 8192));
  // Fresh worker is alive with generous timeout.
  EXPECT_TRUE(state.DetectDeadWorkers(std::chrono::seconds(10)).empty());
  // Zero timeout plus a sleep past the 1s granularity forces expiry deterministically.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  auto dead = state.DetectDeadWorkers(std::chrono::seconds(0));
  ASSERT_EQ(dead.size(), 1u);
  EXPECT_EQ(dead[0], "w1");
  // Already-dead workers are not reported twice.
  EXPECT_TRUE(state.DetectDeadWorkers(std::chrono::seconds(0)).empty());
}

TEST(ClusterState, DeadWorkerExcludedFromScheduling) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 4, 8192));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  state.DetectDeadWorkers(std::chrono::seconds(0));
  Scheduler s;
  EXPECT_FALSE(s.PickWorker(MakeJob("j", 1, 256), state).has_value());
}

TEST(ClusterState, JobsOnWorkerListsForReschedule) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 4, 8192));
  state.RegisterWorker(MakeWorker("w2", 4, 8192));
  JobRecord a = MakeJob("a", 1, 256);
  a.assigned_worker_id = "w1";
  JobRecord b = MakeJob("b", 1, 256);
  b.assigned_worker_id = "w2";
  state.AddJob(a);
  state.AddJob(b);
  auto jobs = state.JobsOnWorker("w1");
  ASSERT_EQ(jobs.size(), 1u);
  EXPECT_EQ(jobs[0].job_id, "a");
}

TEST(ClusterState, AssignReservesCapacityLocally) {
  ClusterState state;
  state.RegisterWorker(MakeWorker("w1", 4, 1024));
  JobRecord j = MakeJob("j", 1, 256);
  state.AddJob(j);
  state.AssignJobToWorker("j", "w1");
  WorkerRecord w;
  ASSERT_TRUE(state.GetWorker("w1", &w));
  EXPECT_EQ(w.cpu_available, 3);
  EXPECT_EQ(w.mem_available_mb, 768);
}

TEST(ClusterState, PendingJobsOnlyListsPending) {
  ClusterState state;
  JobRecord a = MakeJob("a", 1, 256);
  JobRecord b = MakeJob("b", 1, 256);
  b.state = JobDesiredState::RUNNING;
  state.AddJob(a);
  state.AddJob(b);
  auto pending = state.PendingJobs();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].job_id, "a");
}
