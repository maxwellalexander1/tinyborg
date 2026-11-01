#include <gtest/gtest.h>
#include "job_runner.h"
#include <chrono>
#include <thread>

using namespace tinyborg;

static bool WaitForState(JobRunner& r, const std::string& id, JobState want, int timeout_ms = 5000) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    r.ReapExited();
    RunningJob j;
    if (r.GetJob(id, &j) && j.state == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  r.ReapExited();
  RunningJob j;
  return r.GetJob(id, &j) && j.state == want;
}

TEST(JobRunner, NormalExitReportsExited) {
  JobRunner r;
  ASSERT_TRUE(r.StartJob("t1", "/bin/true", {}));
  ASSERT_TRUE(WaitForState(r, "t1", JobState::EXITED));
  RunningJob j;
  ASSERT_TRUE(r.GetJob("t1", &j));
  EXPECT_EQ(j.exit_code, 0);
}

TEST(JobRunner, ShortSleepRunsThenExits) {
  JobRunner r;
  ASSERT_TRUE(r.StartJob("s1", "/bin/sleep", {"0.2"}));
  RunningJob j;
  ASSERT_TRUE(r.GetJob("s1", &j));
  EXPECT_EQ(j.state, JobState::RUNNING);
  ASSERT_TRUE(WaitForState(r, "s1", JobState::EXITED));
}

TEST(JobRunner, KillJobTerminatesSleep) {
  JobRunner r;
  ASSERT_TRUE(r.StartJob("k1", "/bin/sleep", {"30"}));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(r.KillJob("k1"));
  ASSERT_TRUE(WaitForState(r, "k1", JobState::KILLED));
}

TEST(JobRunner, KillUnknownJobReturnsFalse) {
  JobRunner r;
  EXPECT_FALSE(r.KillJob("nope"));
}

TEST(JobRunner, BadBinaryExits127) {
  JobRunner r;
  ASSERT_TRUE(r.StartJob("bad", "/nonexistent-binary-xyz", {}));
  ASSERT_TRUE(WaitForState(r, "bad", JobState::EXITED));
  RunningJob j;
  ASSERT_TRUE(r.GetJob("bad", &j));
  EXPECT_EQ(j.exit_code, 127);
  // NOTE: header declares FAILED_TO_START but impl never sets it.
  // Bad binaries surface as EXITED 127. Honest gap, not fixed here.
}
