#include "scheduler.h"
#include <algorithm>

namespace tinyborg {

std::optional<SchedulingDecision> Scheduler::PickWorker(
    const JobRecord& job, const ClusterState& state) const {
  std::vector<WorkerRecord> workers = state.AllAliveWorkers();

  // ---- Pass 1: first-fit, no preemption needed ----
  for (const auto& worker : workers) {
    if (worker.cpu_available >= job.cpu_request &&
        worker.mem_available_mb >= job.mem_request_mb) {
      return SchedulingDecision{worker.worker_id, {}};
    }
  }

  // ---- Pass 2: try preemption. Find a worker where killing some
  // lower-priority jobs would free up enough room. ----
  for (const auto& worker : workers) {
    std::vector<JobRecord> worker_jobs = state.JobsOnWorker(worker.worker_id);

    // Only consider jobs with strictly lower priority than the incoming job.
    std::vector<JobRecord> candidates;
    for (const auto& j : worker_jobs) {
      if (j.priority < job.priority) {
        candidates.push_back(j);
      }
    }
    if (candidates.empty()) continue;

    // Sort lowest priority first, so we preempt the least important jobs first.
    std::sort(candidates.begin(), candidates.end(),
              [](const JobRecord& a, const JobRecord& b) {
                return a.priority < b.priority;
              });

    int32_t freed_cpu = worker.cpu_available;
    int64_t freed_mem = worker.mem_available_mb;
    std::vector<std::string> to_preempt;

    for (const auto& candidate : candidates) {
      if (freed_cpu >= job.cpu_request && freed_mem >= job.mem_request_mb) {
        break;  // already enough, stop preempting more than necessary
      }
      freed_cpu += candidate.cpu_request;
      freed_mem += candidate.mem_request_mb;
      to_preempt.push_back(candidate.job_id);
    }

    if (freed_cpu >= job.cpu_request && freed_mem >= job.mem_request_mb) {
      return SchedulingDecision{worker.worker_id, to_preempt};
    }
  }

  return std::nullopt;  // no worker, even with preemption, has room
}

}  // namespace tinyborg
