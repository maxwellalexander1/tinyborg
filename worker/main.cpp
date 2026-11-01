#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"
#include "job_runner.h"
#include "../common/tls.h"
#include "../common/manifest.h"
#include <ctime>

#include <chrono>
#include <thread>
#include <iostream>
#include <atomic>
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
    if (!tinyborg::tls::UseInsecure()) {
      std::string cn = tinyborg::tls::PeerCN(context);
      if (cn != "master") {
        return Status(grpc::PERMISSION_DENIED, "only master may start jobs");
      }
    }
    const auto& spec = request->spec();

    std::vector<std::string> args(spec.args().begin(), spec.args().end());

    std::lock_guard<std::mutex> lock(mutex_);
    // Signed manifest check (T9): refuse unsigned, tampered, expired, replayed.
    {
      std::string trusted_pub_path = tinyborg::tls::GetEnvOr(
          "TINYBORG_TRUSTED_PUBKEY", "certs/submit/submitter.pub");
      std::string trusted_id = tinyborg::tls::GetEnvOr("TINYBORG_TRUSTED_KEY_ID", "default");
      int64_t now = (int64_t)std::time(nullptr);
      for (auto it = seen_jobs_.begin(); it != seen_jobs_.end();) {
        if (now >= it->second) it = seen_jobs_.erase(it);
        else ++it;
      }
      std::string why;
      if (spec.signature().empty()) {
        why = "unsigned manifest";
      } else if (spec.submitter_key_id() != trusted_id) {
        why = "unknown key id";
      } else if (!(spec.issued_at_unix() <= now + 60 && now < spec.expires_at_unix() &&
                   spec.issued_at_unix() <= spec.expires_at_unix())) {
        why = "expired or bad timestamps";
      } else if (seen_jobs_.count(spec.job_id())) {
        why = "replayed job id";
      } else {
        std::string pub = tinyborg::tls::ReadFile(trusted_pub_path);
        std::string msg = tinyborg::manifest::Canonical(
            spec.job_id(), spec.binary_path(),
            tinyborg::manifest::JoinArgs(args), spec.cpu_request(),
            spec.mem_request_mb(), spec.priority(), spec.issued_at_unix(),
            spec.expires_at_unix(), spec.submitter_key_id());
        if (!tinyborg::manifest::Verify(pub, msg, spec.signature())) {
          why = "bad signature";
        }
      }
      if (!why.empty()) {
        std::cerr << "refusing job " << spec.job_id() << ": " << why << "\n";
        reply->set_success(false);
        reply->set_message(std::string("refused: ") + why);
        return Status::OK;
      }
      seen_jobs_[spec.job_id()] = spec.expires_at_unix();
    }
    bool ok = runner_.StartJob(spec.job_id(), spec.binary_path(), args);

    if (ok) {
      // Track this job's resource allocation so heartbeats report real
      // available capacity, not a static constant.
      allocations_[spec.job_id()] = {spec.cpu_request(), spec.mem_request_mb()};
      generations_[spec.job_id()] = request->generation();
    }

    reply->set_success(ok);
    reply->set_message(ok ? "started" : "failed to start");
    return Status::OK;
  }

  Status KillJob(ServerContext* context,
                 const tinyborg::JobId* request,
                 tinyborg::JobAck* reply) override {
    if (!tinyborg::tls::UseInsecure()) {
      std::string cn = tinyborg::tls::PeerCN(context);
      if (cn != "master") {
        return Status(grpc::PERMISSION_DENIED, "only master may kill jobs");
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = runner_.KillJob(request->job_id());

    reply->set_success(ok);
    reply->set_message(ok ? "kill signal sent" : "job not found or not running");
    return Status::OK;
  }

  Status GetJobStatus(ServerContext* context,
                       const tinyborg::JobId* request,
                       tinyborg::JobStatus* reply) override {
    if (!tinyborg::tls::UseInsecure()) {
      std::string cn = tinyborg::tls::PeerCN(context);
      if (cn != "master") {
        return Status(grpc::PERMISSION_DENIED, "only master may query jobs");
      }
    }
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
    auto git = generations_.find(job.job_id);
    reply->set_generation(git != generations_.end() ? git->second : 0);

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
  std::unordered_map<std::string, uint64_t> generations_;
  std::unordered_map<std::string, int64_t> seen_jobs_;

 public:
  // Lease expiry: kill all local jobs. Called by lease monitor, not RPC.
  void KillAllLocalJobs() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, job] : allocations_) {
      (void)id;
    }
    // Collect ids first to avoid mutating while iterating runner state.
    std::vector<std::string> ids;
    for (const auto& [id, a] : allocations_) ids.push_back(id);
    for (const auto& id : ids) {
      runner_.KillJob(id);
    }
    runner_.ReapExited();
    ReleaseFinishedAllocations();
  }
};

class MasterClient {
 public:
  explicit MasterClient(const std::string& master_address,
                        const std::string& worker_id) {
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (tinyborg::tls::UseInsecure()) {
      creds = grpc::InsecureChannelCredentials();
    } else {
      std::string ca = tinyborg::tls::GetEnvOr("TINYBORG_CA_CRT", "certs/ca.crt");
      std::string crt = tinyborg::tls::GetEnvOr("TINYBORG_CLIENT_CRT", "");
      std::string key = tinyborg::tls::GetEnvOr("TINYBORG_CLIENT_KEY", "");
      if (crt.empty()) crt = "certs/" + worker_id + ".crt";
      if (key.empty()) key = "certs/" + worker_id + ".key";
      creds = tinyborg::tls::ClientCreds(ca, crt, key);
    }
    auto channel = grpc::CreateChannel(master_address, creds);
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

  // Returns lease timeout from master, or 0 if heartbeat failed.
  int SendHeartbeat(const std::string& worker_id,
                      int32_t cpu_available, int64_t mem_available_mb) {
    tinyborg::HeartbeatRequest request;
    request.set_worker_id(worker_id);
    request.set_cpu_available(cpu_available);
    request.set_mem_available_mb(mem_available_mb);

    tinyborg::HeartbeatAck reply;
    grpc::ClientContext context;
    grpc::Status status = stub_->Heartbeat(&context, request, &reply);
    if (!status.ok()) return 0;
    return reply.lease_timeout_sec();
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
  if (tinyborg::tls::UseInsecure()) {
    tinyborg::tls::WarnInsecure();
    builder.AddListeningPort(my_address, grpc::InsecureServerCredentials());
  } else {
    std::string ca = tinyborg::tls::GetEnvOr("TINYBORG_CA_CRT", "certs/ca.crt");
    std::string crt = tinyborg::tls::GetEnvOr("TINYBORG_SERVER_CRT", "");
    std::string key = tinyborg::tls::GetEnvOr("TINYBORG_SERVER_KEY", "");
    if (crt.empty()) crt = "certs/" + worker_id + ".crt";
    if (key.empty()) key = "certs/" + worker_id + ".key";
    builder.AddListeningPort(my_address, tinyborg::tls::ServerCreds(ca, crt, key));
  }
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Worker " << worker_id << " listening on " << my_address << "\n";

  MasterClient master_client(master_address, worker_id);
  bool registered = master_client.RegisterWorker(worker_id, my_address, kCpuCores, kMemMb);
  std::cout << "Registered with master: " << (registered ? "success" : "FAILED") << "\n";

  std::atomic<int64_t> last_ack_ms{std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()};
  std::atomic<int> lease_sec{5};

  std::thread heartbeat_thread([&]() {
    while (true) {
      auto [cpu_available, mem_available] = service.GetAvailableResources();
      int lease = master_client.SendHeartbeat(worker_id, cpu_available, mem_available);
      if (lease > 0) {
        lease_sec.store(lease);
        last_ack_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  });
  heartbeat_thread.detach();

  // Lease monitor: if master stops renewing (partition), kill local jobs.
  // Lease must be shorter than master's dead timeout so zombie stops first.
  std::thread lease_thread([&]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t age_ms = now_ms - last_ack_ms.load();
      if (age_ms > (int64_t)lease_sec.load() * 1000) {
        std::cout << "Lease expired (no ack for " << age_ms << "ms), killing local jobs\n";
        service.KillAllLocalJobs();
        // Avoid repeat-kill spam: reset clock, will fire again if still partitioned.
        last_ack_ms.store(now_ms);
      }
    }
  });
  lease_thread.detach();

  server->Wait();
}

int main(int argc, char** argv) {
  std::string worker_id, my_address, master_address;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--insecure") {
      setenv("TINYBORG_INSECURE", "1", 1);
    } else if (worker_id.empty()) {
      worker_id = argv[i];
    } else if (my_address.empty()) {
      my_address = argv[i];
    } else if (master_address.empty()) {
      master_address = argv[i];
    }
  }
  if (master_address.empty()) {
    std::cerr << "Usage: " << argv[0] << " <worker_id> <my_address> <master_address> [--insecure]\n";
    return 1;
  }
  RunServer(worker_id, my_address, master_address);
  return 0;
}
