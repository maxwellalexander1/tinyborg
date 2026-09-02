#include "cluster_state.h"

namespace tinyborg {

void ClusterState::RegisterWorker(const WorkerRecord& worker) {
  std::lock_guard<std::mutex> lock(mutex_);
  WorkerRecord w = worker;
  w.last_heartbeat = std::chrono::steady_clock::now();
  w.alive = true;
  workers_[worker.worker_id] = w;
}

void ClusterState::UpdateHeartbeat(const std::string& worker_id,
                                    int32_t cpu_available,
                                    int64_t mem_available_mb) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workers_.find(worker_id);
  if (it == workers_.end()) {
    return;  // unknown worker, ignore (or could log this as suspicious)
  }
  it->second.cpu_available = cpu_available;
  it->second.mem_available_mb = mem_available_mb;
  it->second.last_heartbeat = std::chrono::steady_clock::now();
  it->second.alive = true;
}

// TODO(stretch goal): replace fixed timeout with adaptive detection,
// modeled on TCP RTO estimation (rolling avg + variance of heartbeat
// intervals per worker, timeout = avg + 4*stddev instead of a global constant)
std::vector<std::string> ClusterState::DetectDeadWorkers(
    std::chrono::seconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> newly_dead;
  auto now = std::chrono::steady_clock::now();

  for (auto& [id, worker] : workers_) {
    if (!worker.alive) continue;  // already marked dead, skip

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - worker.last_heartbeat);
    if (elapsed > timeout) {
      worker.alive = false;
      newly_dead.push_back(id);
    }
  }

  return newly_dead;
}

bool ClusterState::GetWorker(const std::string& worker_id,
                              WorkerRecord* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = workers_.find(worker_id);
  if (it == workers_.end()) return false;
  *out = it->second;
  return true;
}

std::vector<WorkerRecord> ClusterState::AllAliveWorkers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WorkerRecord> result;
  for (const auto& [id, worker] : workers_) {
    if (worker.alive) result.push_back(worker);
  }
  return result;
}

void ClusterState::AddJob(const JobRecord& job) {
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_[job.job_id] = job;
}

bool ClusterState::GetJob(const std::string& job_id, JobRecord* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(job_id);
  if (it == jobs_.end()) return false;
  *out = it->second;
  return true;
}

void ClusterState::SetJobState(const std::string& job_id,
                                JobDesiredState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(job_id);
  if (it != jobs_.end()) {
    it->second.state = state;
  }
}

void ClusterState::AssignJobToWorker(const std::string& job_id,
                                      const std::string& worker_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(job_id);
  if (it != jobs_.end()) {
    it->second.assigned_worker_id = worker_id;
    it->second.state = JobDesiredState::SCHEDULED;
  }
}

std::vector<JobRecord> ClusterState::PendingJobs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JobRecord> result;
  for (const auto& [id, job] : jobs_) {
    if (job.state == JobDesiredState::PENDING) result.push_back(job);
  }
  return result;
}

std::vector<JobRecord> ClusterState::JobsOnWorker(
    const std::string& worker_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<JobRecord> result;
  for (const auto& [id, job] : jobs_) {
    if (job.assigned_worker_id == worker_id) result.push_back(job);
  }
  return result;
}

}  // namespace tinyborg
