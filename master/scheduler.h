#pragma once

#include <string>
#include <optional>
#include "cluster_state.h"

namespace tinyborg {

class Scheduler {
 public:
  std::optional<std::string> PickWorker(const JobRecord& job,
                                         const ClusterState& state) const;
};

}  // namespace tinyborg
