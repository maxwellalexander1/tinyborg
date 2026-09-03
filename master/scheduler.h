#pragma once

#include <string>
#include <optional>
#include <vector>
#include "cluster_state.h"

namespace tinyborg {

struct SchedulingDecision {
  std::string worker_id;
  // Jobs that need to be preempted (killed + rescheduled) on that worker
  // to make room. Empty if the job fit without preemption.
  std::vector<std::string> jobs_to_preempt;
};

class Scheduler {
 public:
  // Given a job's resource requirements and the current cluster state,
  // pick a worker to run it on, using preemption of lower-priority jobs
  // if no worker has free capacity outright.
  std::optional<SchedulingDecision> PickWorker(const JobRecord& job,
                                                const ClusterState& state) const;
};

}  // namespace tinyborg
