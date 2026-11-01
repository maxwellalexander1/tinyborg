#include <grpcpp/grpcpp.h>
#include "borg.grpc.pb.h"
#include "../common/tls.h"
#include "../common/manifest.h"
#include <ctime>

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

      // Signed manifest (T9): key id + issued/expiry + Ed25519 signature.
      std::string key_id = tinyborg::tls::GetEnvOr("TINYBORG_SUBMIT_KEY_ID", "default");
      std::string key_path = tinyborg::tls::GetEnvOr("TINYBORG_SUBMIT_KEY", "");
      int64_t now = (int64_t)std::time(nullptr);
      int64_t ttl = 300;
      if (const char* e = std::getenv("TINYBORG_MANIFEST_TTL_SEC")) {
        try { ttl = std::stoll(e); } catch (...) {}
      }
      request.set_submitter_key_id(key_id);
      request.set_issued_at_unix(now);
      request.set_expires_at_unix(now + ttl);
      if (!key_path.empty()) {
        std::string priv = tinyborg::tls::ReadFile(key_path);
        std::string msg = tinyborg::manifest::Canonical(
            request.job_id(), request.binary_path(),
            tinyborg::manifest::JoinArgs(args), request.cpu_request(),
            request.mem_request_mb(), request.priority(),
            request.issued_at_unix(), request.expires_at_unix(), key_id);
        std::string sig;
        if (!tinyborg::manifest::Sign(priv, msg, &sig)) {
          std::cerr << "signing failed\n";
          return 1;
        }
        request.set_signature(sig);
      }

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
      std::cout << "generation: " << reply.generation() << "\n";
      return 0;

  } else {
    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage(argv[0]);
    return 1;
  }
}
