#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"

#include <iostream>
#include <string>
#include <vector>

void PrintUsage(const char* prog) {
  std::cerr << "Usage:\n"
            << "  " << prog << " submit <job_id> <binary_path> [args...]\n"
            << "  " << prog << " status <job_id>\n"
            << "\n"
            << "Example:\n"
            << "  " << prog << " submit job1 /bin/sleep 10\n"
            << "  " << prog << " status job1\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::string command = argv[1];
  std::string master_address = "localhost:50051";

  auto channel = grpc::CreateChannel(master_address,
                                      grpc::InsecureChannelCredentials());
  auto stub = tinyborg::Master::NewStub(channel);

  if (command == "submit") {
    if (argc < 4) {
      PrintUsage(argv[0]);
      return 1;
    }

    tinyborg::JobSpec request;
    request.set_job_id(argv[2]);
    request.set_binary_path(argv[3]);
    for (int i = 4; i < argc; i++) {
      request.add_args(argv[i]);
    }
    // Hardcoded resource request for now, could be flags later
    request.set_cpu_request(1);
    request.set_mem_request_mb(256);
    request.set_priority(0);

    tinyborg::JobAck reply;
    grpc::ClientContext context;
    grpc::Status status = stub->SubmitJob(&context, request, &reply);

    if (!status.ok()) {
      std::cerr << "RPC failed: " << status.error_message() << "\n";
      return 1;
    }

    std::cout << (reply.success() ? "OK: " : "FAILED: ") << reply.message() << "\n";
    return reply.success() ? 0 : 1;

  } else if (command == "status") {
      if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
      }

      tinyborg::JobId request;
      request.set_job_id(argv[2]);

      tinyborg::JobStatus reply;
      grpc::ClientContext context;
      grpc::Status status = stub->GetJobStatus(&context, request, &reply);

      if (!status.ok()) {
        std::cerr << "RPC failed: " << status.error_message() << "\n";
        return 1;
      }

      std::cout << "job_id: " << reply.job_id() << "\n";
      std::cout << "phase: " << tinyborg::JobPhase_Name(reply.phase()) << "\n";
      if (reply.pid() > 0) {
        std::cout << "pid: " << reply.pid() << "\n";
      }
      if (!reply.worker_id().empty()) {
        std::cout << "worker: " << reply.worker_id() << "\n";
      }
      return 0;

  } else {
    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage(argv[0]);
    return 1;
  }
}
