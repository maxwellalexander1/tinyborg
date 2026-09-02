// master/worker_client.cpp
#include "worker_client.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

namespace tinyborg {

WorkerClient::WorkerClient(const std::string& worker_address) {
  auto channel = grpc::CreateChannel(worker_address,
                                      grpc::InsecureChannelCredentials());
  stub_ = Worker::NewStub(channel);
}

bool WorkerClient::StartJob(const JobRecord& job) {
  JobAssignment request;
  JobSpec* spec = request.mutable_spec();
  spec->set_job_id(job.job_id);
  spec->set_binary_path(job.binary_path);
  for (const auto& arg : job.args) {
    spec->add_args(arg);
  }
  spec->set_cpu_request(job.cpu_request);
  spec->set_mem_request_mb(job.mem_request_mb);
  spec->set_priority(job.priority);

  JobAck reply;
  grpc::ClientContext context;
  grpc::Status status = stub_->StartJob(&context, request, &reply);

  if (!status.ok()) {
    std::cerr << "StartJob RPC failed for " << job.job_id << ": "
              << status.error_message() << "\n";
    return false;
  }
  return reply.success();
}

}  // namespace tinyborg
