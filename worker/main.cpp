#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"
#include "job_runner.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <mutex>
#include <cstdlib>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

class WorkerServiceImpl final : public tinyborg::Worker::Service {
 public:
  Status StartJob(ServerContext* context,
                   const tinyborg::JobAssignment* request,
                   tinyborg::JobAck* reply) override {
    const auto& spec = request->spec();

    std::vector<std::string> args(spec.args().begin(), spec.args().end());

    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = runner_.StartJob(spec.job_id(), spec.binary_path(), args);

    reply->set_success(ok);
    reply->set_message(ok ? "started" : "failed to start");
    return Status::OK;
  }

  Status KillJob(ServerContext* context,
                 const tinyborg::JobId* request,
                 tinyborg::JobAck* reply) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = runner_.KillJob(request->job_id());

    reply->set_success(ok);
    reply->set_message(ok ? "kill signal sent" : "job not found or not running");
    return Status::OK;
  }

  Status GetJobStatus(ServerContext* context,
                       const tinyborg::JobId* request,
                       tinyborg::JobStatus* reply) override {
    std::lock_guard<std::mutex> lock(mutex_);
    runner_.ReapExited();  // refresh state before reporting

    tinyborg::RunningJob job;
    if (!runner_.GetJob(request->job_id(), &job)) {
      reply->set_job_id(request->job_id());
      reply->set_phase(tinyborg::UNKNOWN);
      return Status::OK;
    }

    reply->set_job_id(job.job_id);
    reply->set_pid(job.pid);

    switch (job.state) {
      case tinyborg::JobState::RUNNING:
        reply->set_phase(tinyborg::RUNNING);
        break;
      case tinyborg::JobState::EXITED:
        reply->set_phase(tinyborg::COMPLETED);
        break;
      case tinyborg::JobState::KILLED:
      case tinyborg::JobState::FAILED_TO_START:
        reply->set_phase(tinyborg::FAILED);
        break;
    }

    return Status::OK;
  }

  // Called periodically from a background thread to reap exited children,
  // independent of whether anyone's actively polling status.
  void ReapLoop() {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        runner_.ReapExited();
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

 private:
  tinyborg::JobRunner runner_;
  std::mutex mutex_;
};

// Thin client for this worker to talk to the master (register + heartbeat).
class MasterClient {
 public:
  explicit MasterClient(const std::string& master_address) {
    auto channel = grpc::CreateChannel(master_address,
                                        grpc::InsecureChannelCredentials());
    stub_ = tinyborg::Master::NewStub(channel);
  }

  bool RegisterWorker(const std::string& worker_id,
                       const std::string& my_address,
                       int32_t cpu_cores, int64_t mem_mb) {
    tinyborg::WorkerInfo request;
    request.set_worker_id(worker_id);
    request.set_address(my_address);
    request.set_cpu_cores(cpu_cores);
    request.set_mem_mb(mem_mb);

    tinyborg::RegisterAck reply;
    grpc::ClientContext context;
    grpc::Status status = stub_->RegisterWorker(&context, request, &reply);
    return status.ok() && reply.success();
  }

  void SendHeartbeat(const std::string& worker_id,
                      int32_t cpu_available, int64_t mem_available_mb) {
    tinyborg::HeartbeatRequest request;
    request.set_worker_id(worker_id);
    request.set_cpu_available(cpu_available);
    request.set_mem_available_mb(mem_available_mb);

    tinyborg::HeartbeatAck reply;
    grpc::ClientContext context;
    stub_->Heartbeat(&context, request, &reply);  // fire and forget, ignore failures for now
  }

 private:
  std::unique_ptr<tinyborg::Master::Stub> stub_;
};

void RunServer(const std::string& worker_id,
               const std::string& my_address,
               const std::string& master_address) {
  WorkerServiceImpl service;

  std::thread reap_thread(&WorkerServiceImpl::ReapLoop, &service);
  reap_thread.detach();

  ServerBuilder builder;
  builder.AddListeningPort(my_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Worker " << worker_id << " listening on " << my_address << "\n";

  // Register with master, hardcoded fake capacity for now (4 cores, 8GB)
  MasterClient master_client(master_address);
  const int32_t kCpuCores = 4;
  const int64_t kMemMb = 8192;

  bool registered = master_client.RegisterWorker(worker_id, my_address, kCpuCores, kMemMb);
  std::cout << "Registered with master: " << (registered ? "success" : "FAILED") << "\n";

  // Heartbeat loop, every 2 seconds, in a detached thread
  std::thread heartbeat_thread([&]() {
    while (true) {
      master_client.SendHeartbeat(worker_id, kCpuCores, kMemMb);  // TODO: report real usage
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  });
  heartbeat_thread.detach();

  server->Wait();
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " <worker_id> <my_address> <master_address>\n";
    std::cerr << "Example: " << argv[0] << " worker1 0.0.0.0:50052 localhost:50051\n";
    return 1;
  }
  RunServer(argv[1], argv[2], argv[3]);
  return 0;
}
