// master/master_service.cpp
#include "master_service.h"
#include "../common/tls.h"
#include <grpcpp/grpcpp.h>

namespace tinyborg {

grpc::Status MasterServiceImpl::RegisterWorker(grpc::ServerContext* context,
                                                const WorkerInfo* request,
                                                RegisterAck* reply) {
  if (!tls::UseInsecure()) {
    std::string cn = tls::PeerCN(context);
    if (!tls::CNIsWorker(cn)) {
      return grpc::Status(grpc::PERMISSION_DENIED, "only workers may register");
    }
  }
  WorkerRecord w;
  w.worker_id = request->worker_id();
  w.address = request->address();
  w.cpu_cores = request->cpu_cores();
  w.mem_mb = request->mem_mb();
  w.cpu_available = request->cpu_cores();   // starts fully free
  w.mem_available_mb = request->mem_mb();

  state_->RegisterWorker(w);
  reply->set_success(true);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::Heartbeat(grpc::ServerContext* context,
                                           const HeartbeatRequest* request,
                                           HeartbeatAck* reply) {
  if (!tls::UseInsecure()) {
    std::string cn = tls::PeerCN(context);
    if (!tls::CNIsWorker(cn)) {
      return grpc::Status(grpc::PERMISSION_DENIED, "only workers may heartbeat");
    }
  }
  state_->UpdateHeartbeat(request->worker_id(),
                           request->cpu_available(),
                           request->mem_available_mb());
  reply->set_acknowledged(true);
  int lease_sec = 5;
  if (const char* env = std::getenv("TINYBORG_LEASE_SEC")) {
    try { lease_sec = std::stoi(env); } catch (...) {}
  }
  reply->set_lease_timeout_sec(lease_sec);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::SubmitJob(grpc::ServerContext* context,
                                           const JobSpec* request,
                                           JobAck* reply) {
  if (!tls::UseInsecure()) {
    std::string cn = tls::PeerCN(context);
    if (cn != "tbctl-client") {
      return grpc::Status(grpc::PERMISSION_DENIED, "only tbctl-client may submit");
    }
  }
  JobRecord job;
  job.job_id = request->job_id();
  job.binary_path = request->binary_path();
  job.args.assign(request->args().begin(), request->args().end());
  job.cpu_request = request->cpu_request();
  job.mem_request_mb = request->mem_request_mb();
  job.priority = request->priority();
  job.state = JobDesiredState::PENDING;

  state_->AddJob(job);

  reply->set_success(true);
  reply->set_message("job accepted, pending scheduling");
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::GetJobStatus(grpc::ServerContext* context,
                                              const JobId* request,
                                              JobStatus* reply) {
  if (!tls::UseInsecure()) {
    std::string cn = tls::PeerCN(context);
    if (cn != "tbctl-client") {
      return grpc::Status(grpc::PERMISSION_DENIED, "only tbctl-client may query");
    }
  }
  JobRecord job;
  if (!state_->GetJob(request->job_id(), &job)) {
    reply->set_job_id(request->job_id());
    reply->set_phase(JobPhase::UNKNOWN);
    return grpc::Status::OK;
  }

  if (job.assigned_worker_id.empty()) {
    // Job exists but hasn't been scheduled to a worker yet
    reply->set_job_id(job.job_id);
    reply->set_phase(JobPhase::PENDING);
    reply->set_worker_id("");
    return grpc::Status::OK;
  }

  WorkerRecord worker;
  if (!state_->GetWorker(job.assigned_worker_id, &worker)) {
    // Shouldn't happen, but guard anyway
    reply->set_job_id(job.job_id);
    reply->set_phase(JobPhase::UNKNOWN);
    return grpc::Status::OK;
  }

  // Proxy the actual query to the worker that's running this job
  std::shared_ptr<grpc::ChannelCredentials> wcreds;
  if (tls::UseInsecure()) {
    wcreds = grpc::InsecureChannelCredentials();
  } else {
    std::string ca = tls::GetEnvOr("TINYBORG_CA_CRT", "certs/ca.crt");
    wcreds = tls::ClientCreds(ca, tls::GetEnvOr("TINYBORG_CLIENT_CRT", "certs/master.crt"),
                              tls::GetEnvOr("TINYBORG_CLIENT_KEY", "certs/master.key"));
  }
  auto channel = grpc::CreateChannel(worker.address, wcreds);
  auto stub = Worker::NewStub(channel);

  JobId worker_request;
  worker_request.set_job_id(job.job_id);

  grpc::ClientContext worker_context;
  grpc::Status status = stub->GetJobStatus(&worker_context, worker_request, reply);

  if (!status.ok()) {
    reply->set_job_id(job.job_id);
    reply->set_phase(JobPhase::UNKNOWN);
    reply->set_worker_id(job.assigned_worker_id);
    reply->set_generation(job.generation);
    return grpc::Status::OK;
  }

  // Fencing: ignore stale results from a partitioned worker.
  // Worker echoes the generation it was assigned. If older than master's
  // current generation, the job was rescheduled and this late result wins nothing.
  if (reply->generation() != job.generation) {
    reply->set_job_id(job.job_id);
    reply->set_phase(JobPhase::UNKNOWN);
    reply->set_worker_id(job.assigned_worker_id);
    reply->set_generation(job.generation);
    return grpc::Status::OK;
  }

  // Preserve which worker the master thinks owns this job.
  reply->set_worker_id(job.assigned_worker_id);
  return grpc::Status::OK;
}

}  // namespace tinyborg
