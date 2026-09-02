// master/master_service.cpp
#include "master_service.h"
#include <grpcpp/grpcpp.h>

namespace tinyborg {

grpc::Status MasterServiceImpl::RegisterWorker(grpc::ServerContext* context,
                                                const WorkerInfo* request,
                                                RegisterAck* reply) {
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
  state_->UpdateHeartbeat(request->worker_id(),
                           request->cpu_available(),
                           request->mem_available_mb());
  reply->set_acknowledged(true);
  return grpc::Status::OK;
}

grpc::Status MasterServiceImpl::SubmitJob(grpc::ServerContext* context,
                                           const JobSpec* request,
                                           JobAck* reply) {
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
  auto channel = grpc::CreateChannel(worker.address,
                                      grpc::InsecureChannelCredentials());
  auto stub = Worker::NewStub(channel);

  JobId worker_request;
  worker_request.set_job_id(job.job_id);

  grpc::ClientContext worker_context;
  grpc::Status status = stub->GetJobStatus(&worker_context, worker_request, reply);

  if (!status.ok()) {
    reply->set_job_id(job.job_id);
    reply->set_phase(JobPhase::UNKNOWN);
    return grpc::Status::OK;
  }

  return grpc::Status::OK;
}

}  // namespace tinyborg
