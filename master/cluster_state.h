#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>

namespace tinyborg {

struct WorkerRecord {
  std::string worker_id;
  std::string address;       // host:port for master -> worker calls
  int32_t cpu_cores = 0;
  int64_t mem_mb = 0;

  int32_t cpu_available = 0;
  int64_t mem_available_mb = 0;

  std::chrono::steady_clock::time_point last_heartbeat;
  bool alive = true;
};

enum class JobDesiredState {
  PENDING,     // submitted, not yet scheduled to a worker
  SCHEDULED,   // assigned to a worker, StartJob RPC sent
  RUNNING,     // worker confirmed it's running
  FAILED,      // worker reported failure, or worker died
  COMPLETED    // job exited normally
};

struct JobRecord {
  std::string job_id;
  std::string binary_path;
  std::vector<std::string> args;
  int32_t cpu_request = 0;
  int64_t mem_request_mb = 0;
  int32_t priority = 0;

  std::string assigned_worker_id;
  JobDesiredState state = JobDesiredState::PENDING;
  uint64_t generation = 0;
  std::string submitter_key_id;
  int64_t issued_at_unix = 0;
  int64_t expires_at_unix = 0;
  std::string signature;
};

// Thread-safe registry of workers and jobs. This is the single source of
// truth the scheduler, gRPC handlers, and health checker all read/write.
class ClusterState {
 public:
  void RegisterWorker(const WorkerRecord& worker);
  void UpdateHeartbeat(const std::string& worker_id,
                        int32_t cpu_available,
                        int64_t mem_available_mb);

  // Returns workers that haven't heartbeat within timeout, marks them
  // not-alive as a side effect.
  std::vector<std::string> DetectDeadWorkers(std::chrono::seconds timeout);

  bool GetWorker(const std::string& worker_id, WorkerRecord* out) const;
  std::vector<WorkerRecord> AllAliveWorkers() const;

  void AddJob(const JobRecord& job);
  bool GetJob(const std::string& job_id, JobRecord* out) const;
  void SetJobState(const std::string& job_id, JobDesiredState state);
  void AssignJobToWorker(const std::string& job_id, const std::string& worker_id);

  // Jobs currently sitting in PENDING, candidates for scheduling.
  std::vector<JobRecord> PendingJobs() const;

  // Jobs assigned to a given worker, used when a worker dies and we need
  // to know what to reschedule.
  std::vector<JobRecord> JobsOnWorker(const std::string& worker_id) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, WorkerRecord> workers_;
  std::unordered_map<std::string, JobRecord> jobs_;
};

}  // namespace tinyborg
