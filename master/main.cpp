// master/main.cpp
#include <grpcpp/grpcpp.h>
#include "cluster_state.h"
#include "scheduler.h"
#include "master_service.h"
#include "worker_client.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <unordered_map>
#include <mutex>

using grpc::Server;
using grpc::ServerBuilder;

namespace tinyborg {

// Background loop: every tick, look for PENDING jobs, try to schedule them,
// and dispatch StartJob to the chosen worker via gRPC.
void SchedulingLoop(ClusterState* state) {
  Scheduler scheduler;
  // Cache WorkerClient per worker_id so we're not reconnecting every tick.
  std::unordered_map<std::string, std::unique_ptr<WorkerClient>> clients;

  while (true) {
    for (const JobRecord& job : state->PendingJobs()) {
      auto worker_id = scheduler.PickWorker(job, *state);
      if (!worker_id.has_value()) {
        continue;  // no capacity yet, try again next tick
      }

      WorkerRecord worker;
      if (!state->GetWorker(*worker_id, &worker)) {
        continue;  // shouldn't happen, but guard anyway
      }

      if (clients.find(*worker_id) == clients.end()) {
        clients[*worker_id] = std::make_unique<WorkerClient>(worker.address);
      }

      bool dispatched = clients[*worker_id]->StartJob(job);
      if (dispatched) {
        state->AssignJobToWorker(job.job_id, *worker_id);
        std::cout << "Scheduled job " << job.job_id
                  << " -> worker " << *worker_id << "\n";
      } else {
        std::cerr << "Failed to dispatch job " << job.job_id
                   << " to worker " << *worker_id << "\n";
        // stays PENDING, will retry next tick (possibly on a different worker)
      }
    }

    // TODO(milestone 3): also call state->DetectDeadWorkers(...) here and
    // reschedule jobs from dead workers back to PENDING.

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  ClusterState state;
  MasterServiceImpl service(&state);

  std::thread scheduling_thread(SchedulingLoop, &state);
  scheduling_thread.detach();

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Master listening on " << server_address << "\n";
  server->Wait();
}

}  // namespace tinyborg

int main() {
  tinyborg::RunServer();
  return 0;
}
