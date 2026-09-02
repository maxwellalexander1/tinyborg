// master/master_service.h
#pragma once
#include "borg.grpc.pb.h"
#include "cluster_state.h"

namespace tinyborg {

class MasterServiceImpl final : public Master::Service {
 public:
  explicit MasterServiceImpl(ClusterState* state) : state_(state) {}

  grpc::Status RegisterWorker(grpc::ServerContext* context,
                               const WorkerInfo* request,
                               RegisterAck* reply) override;

  grpc::Status Heartbeat(grpc::ServerContext* context,
                          const HeartbeatRequest* request,
                          HeartbeatAck* reply) override;

  grpc::Status SubmitJob(grpc::ServerContext* context,
                          const JobSpec* request,
                          JobAck* reply) override;

  grpc::Status GetJobStatus(grpc::ServerContext* context,
                             const JobId* request,
                             JobStatus* reply) override;

 private:
  ClusterState* state_;  // not owned
};

}  // namespace tinyborg
