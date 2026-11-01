#pragma once
// Shared mTLS helpers. Env overrides, file defaults under certs/.
// Insecure fallback via TINYBORG_INSECURE=1 (loud warning, dev only).
#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace tinyborg {
namespace tls {

inline std::string GetEnvOr(const char* name, const std::string& def) {
  if (const char* v = std::getenv(name)) return v;
  return def;
}

inline bool UseInsecure() {
  if (const char* v = std::getenv("TINYBORG_INSECURE")) {
    return std::string(v) == "1";
  }
  return false;
}

inline void WarnInsecure() {
  std::cerr << "WARNING: running with --insecure, no TLS, dev only\n";
}

inline std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline std::shared_ptr<grpc::ServerCredentials> ServerCreds(
    const std::string& ca_path, const std::string& cert_path,
    const std::string& key_path) {
  grpc::SslServerCredentialsOptions opts;
  opts.pem_root_certs = ReadFile(ca_path);
  grpc::SslServerCredentialsOptions::PemKeyCertPair pair;
  pair.private_key = ReadFile(key_path);
  pair.cert_chain = ReadFile(cert_path);
  opts.pem_key_cert_pairs.push_back(pair);
  opts.client_certificate_request =
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  return grpc::SslServerCredentials(opts);
}

inline std::shared_ptr<grpc::ChannelCredentials> ClientCreds(
    const std::string& ca_path, const std::string& cert_path,
    const std::string& key_path) {
  grpc::SslCredentialsOptions opts;
  opts.pem_root_certs = ReadFile(ca_path);
  opts.pem_private_key = ReadFile(key_path);
  opts.pem_cert_chain = ReadFile(cert_path);
  return grpc::SslCredentials(opts);
}

inline std::string PeerCN(grpc::ServerContext* ctx) {
  auto auth = ctx->auth_context();
  if (!auth) return "";
  for (const auto& v : auth->FindPropertyValues("x509_common_name")) {
    return std::string(v.data(), v.size());
  }
  return "";
}

inline bool CNIsWorker(const std::string& cn) {
  return cn.rfind("worker", 0) == 0;
}

}  // namespace tls
}  // namespace tinyborg
