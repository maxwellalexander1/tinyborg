// master/worker_client.cpp
#include "worker_client.h"
#include "../common/tls.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

namespace tinyborg {

WorkerClient::WorkerClient(const std::string& worker_address) {
  std::shared_ptr<grpc::ChannelCredentials> creds;
  if (tls::UseInsecure()) {
    creds = grpc::InsecureChannelCredentials();
  } else {
    std::string ca = tls::GetEnvOr("TINYBORG_CA_CRT", "certs/ca.crt");
    std::string crt = tls::GetEnvOr("TINYBORG_CLIENT_CRT", "certs/master.crt");
    std::string key = tls::GetEnvOr("TINYBORG_CLIENT_KEY", "certs/master.key");
    // Master defaults to its own cert when acting as client to workers.
    if (tls::GetEnvOr("TINYBORG_CLIENT_CRT", "").empty()) crt = "certs/master.crt";
    if (tls::GetEnvOr("TINYBORG_CLIENT_KEY", "").empty()) key = "certs/master.key";
    creds = tls::ClientCreds(ca, crt, key);
  }
  auto channel = grpc::CreateChannel(worker_address, creds);
  stub_ = Worker::NewStub(channel);
}

bool WorkerClient::StartJob(const JobRecord& job, uint64_t generation) {
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
  request.set_generation(generation);

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

bool WorkerClient::KillJob(const std::string& job_id) {
  JobId request;
  request.set_job_id(job_id);

  JobAck reply;
  grpc::ClientContext context;
  grpc::Status status = stub_->KillJob(&context, request, &reply);

  if (!status.ok()) {
    std::cerr << "KillJob RPC failed for " << job_id << ": "
              << status.error_message() << "\n";
    return false;
  }
  return reply.success();
}

}  // namespace tinyborg
