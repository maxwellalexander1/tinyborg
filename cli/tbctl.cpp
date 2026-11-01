#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"
#include "../common/tls.h"

#include <iostream>
#include <string>
#include <cstdlib>
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

  std::string command;
  std::string master_address = "localhost:50051";
  bool insecure = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--insecure") {
      insecure = true;
    } else if (a == "--master" && i + 1 < argc) {
      master_address = argv[++i];
    } else if (command.empty() && a != "submit" && a != "status") {
      // First positional before subcommand is ignored, keep simple.
    }
    if ((a == "submit" || a == "status") && command.empty()) command = a;
  }
  if (command.empty() && argc >= 2) command = argv[1];
  if (!insecure && tinyborg::tls::UseInsecure()) insecure = true;
  if (const char* env = std::getenv("TINYBORG_MASTER_ADDR")) {
    // Env wins unless --master was passed explicitly is complex, keep env.
    master_address = env;
    const std::string prefix = "0.0.0.0:";
    if (master_address.rfind(prefix, 0) == 0) {
      master_address = "localhost:" + master_address.substr(prefix.size());
    }
  }

  std::shared_ptr<grpc::ChannelCredentials> creds;
  if (insecure) {
    std::cerr << "WARNING: running with --insecure, no TLS, dev only\n";
    creds = grpc::InsecureChannelCredentials();
  } else {
    std::string ca = tinyborg::tls::GetEnvOr("TINYBORG_CA_CRT", "certs/ca.crt");
    std::string crt = tinyborg::tls::GetEnvOr("TINYBORG_CLIENT_CRT", "certs/tbctl-client.crt");
    std::string key = tinyborg::tls::GetEnvOr("TINYBORG_CLIENT_KEY", "certs/tbctl-client.key");
    creds = tinyborg::tls::ClientCreds(ca, crt, key);
  }
  auto channel = grpc::CreateChannel(master_address, creds);
  auto stub = tinyborg::Master::NewStub(channel);

  if (command == "submit") {
      if (argc < 4) {
        PrintUsage(argv[0]);
        return 1;
      }

      tinyborg::JobSpec request;
      request.set_job_id(argv[2]);
      request.set_binary_path(argv[3]);

      // Optional: last arg can be "--priority=N", parse it out if present
      int32_t priority = 0;
      std::vector<std::string> args;
      for (int i = 4; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--insecure") continue;
        if (arg == "--master") { i++; continue; }
        if (arg.rfind("--priority=", 0) == 0) {
          priority = std::stoi(arg.substr(11));
        } else {
          args.push_back(arg);
        }
      }
      for (const auto& a : args) {
        request.add_args(a);
      }

      request.set_cpu_request(1);
      request.set_mem_request_mb(256);
      request.set_priority(priority);

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
