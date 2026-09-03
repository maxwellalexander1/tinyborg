#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"
#include "job_runner.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <mutex>
#include <cstdlib>
#include <unordered_map>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

struct ResourceAllocation {
  int32_t cpu = 0;
  int64_t mem_mb = 0;
};

class WorkerServiceImpl final : public tinyborg::Worker::Service {
 public:
  WorkerServiceImpl(int32_t total_cpu, int64_t total_mem_mb)
      : total_cpu_(total_cpu), total_mem_mb_(total_mem_mb) {}

  Status StartJob(ServerContext* context,
                   const tinyborg::JobAssignment* request,
                   tinyborg::JobAck* reply) override {
    const auto& spec = request->spec();

    std::vector<std::string> args(spec.args().begin(), spec.args().end());

    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = runner_.StartJob(spec.job_id(), spec.binary_path(), args);

    if (ok) {
      // Track this job's resource allocation so heartbeats report real
      // available capacity, not a static constant.
      allocations_[spec.job_id()] = {spec.cpu_request(), spec.mem_request_mb()};
    }

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
    runner_.ReapExited();
    ReleaseFinishedAllocations();

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

  // Called periodically to reap exited children AND free their resource
  // allocations, so heartbeats reflect real available capacity.
  void ReapLoop() {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        runner_.ReapExited();
        ReleaseFinishedAllocations();
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // Returns (cpu_available, mem_available_mb) based on total capacity minus
  // what's currently allocated to running jobs.
  std::pair<int32_t, int64_t> GetAvailableResources() {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t used_cpu = 0;
    int64_t used_mem = 0;
    for (const auto& [job_id, alloc] : allocations_) {
      used_cpu += alloc.cpu;
      used_mem += alloc.mem_mb;
    }
    return {total_cpu_ - used_cpu, total_mem_mb_ - used_mem};
  }

 private:
  // Must be called while holding mutex_. Frees allocations for any job
  // that's no longer RUNNING (completed, killed, or failed to start).
  void ReleaseFinishedAllocations() {
    for (auto it = allocations_.begin(); it != allocations_.end();) {
      tinyborg::RunningJob job;
      bool still_tracked = runner_.GetJob(it->first, &job);

      if (!still_tracked || job.state != tinyborg::JobState::RUNNING) {
        it = allocations_.erase(it);
      } else {
        ++it;
      }
    }
  }

  tinyborg::JobRunner runner_;
  std::mutex mutex_;
  int32_t total_cpu_;
  int64_t total_mem_mb_;
  std::unordered_map<std::string, ResourceAllocation> allocations_;
};

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
    stub_->Heartbeat(&context, request, &reply);
  }

 private:
  std::unique_ptr<tinyborg::Master::Stub> stub_;
};

void RunServer(const std::string& worker_id,
               const std::string& my_address,
               const std::string& master_address) {
  const int32_t kCpuCores = 4;
  const int64_t kMemMb = 8192;

  WorkerServiceImpl service(kCpuCores, kMemMb);

  std::thread reap_thread(&WorkerServiceImpl::ReapLoop, &service);
  reap_thread.detach();

  ServerBuilder builder;
  builder.AddListeningPort(my_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Worker " << worker_id << " listening on " << my_address << "\n";

  MasterClient master_client(master_address);
  bool registered = master_client.RegisterWorker(worker_id, my_address, kCpuCores, kMemMb);
  std::cout << "Registered with master: " << (registered ? "success" : "FAILED") << "\n";

  std::thread heartbeat_thread([&]() {
    while (true) {
      auto [cpu_available, mem_available] = service.GetAvailableResources();
      master_client.SendHeartbeat(worker_id, cpu_available, mem_available);
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
