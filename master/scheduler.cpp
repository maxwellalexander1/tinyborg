#include "scheduler.h"

namespace tinyborg {

std::optional<std::string> Scheduler::PickWorker(
    const JobRecord& job, const ClusterState& state) const {
  std::vector<WorkerRecord> workers = state.AllAliveWorkers();

  for (const auto& worker : workers) {
    if (worker.cpu_available >= job.cpu_request &&
        worker.mem_available_mb >= job.mem_request_mb) {
      return worker.worker_id;
    }
  }

  return std::nullopt;  // no worker has room
}

}  // namespace tinyborg
