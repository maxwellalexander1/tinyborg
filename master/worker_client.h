#pragma once
#include <string>
#include "borg.grpc.pb.h"
#include "cluster_state.h"

namespace tinyborg {

class WorkerClient {
 public:
  explicit WorkerClient(const std::string& worker_address);

  bool StartJob(const JobRecord& job);

 private:
  std::unique_ptr<Worker::Stub> stub_;
};

}  // namespace tinyborg
