#pragma once
// Signed manifest helpers (T9). Ed25519 over a canonical byte string.
// Canonical: fields joined by '\n', args joined by '\x1f'. Both sides must match.
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdio>
#include <string>
#include <vector>

namespace tinyborg {
namespace manifest {

inline std::string JoinArgs(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); i++) {
    if (i) out.push_back('\x1f');
    out += args[i];
  }
  return out;
}

inline std::string Canonical(const std::string& job_id,
                             const std::string& binary,
                             const std::string& args_joined,
                             int32_t cpu, int64_t mem, int32_t prio,
                             int64_t issued, int64_t expires,
                             const std::string& key_id) {
  char buf[128];
  std::string out = job_id;
  out.push_back('\n');
  out += binary;
  out.push_back('\n');
  out += args_joined;
  out.push_back('\n');
  snprintf(buf, sizeof(buf), "%d\n%lld\n%d\n%lld\n%lld\n", cpu, (long long)mem,
           prio, (long long)issued, (long long)expires);
  out += buf;
  out += key_id;
  return out;
}

inline bool Sign(const std::string& priv_pem, const std::string& msg,
                 std::string* sig_out) {
  BIO* bio = BIO_new_mem_buf(priv_pem.data(), (int)priv_pem.size());
  if (!bio) return false;
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!pkey) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = false;
  size_t len = 0;
  if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
      EVP_DigestSign(ctx, nullptr, &len, (const unsigned char*)msg.data(),
                     msg.size()) == 1) {
    std::string sig(len, '\0');
    if (EVP_DigestSign(ctx, (unsigned char*)sig.data(), &len,
                        (const unsigned char*)msg.data(), msg.size()) == 1) {
      sig.resize(len);
      *sig_out = sig;
      ok = true;
    }
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ok;
}

inline bool Verify(const std::string& pub_pem, const std::string& msg,
                   const std::string& sig) {
  BIO* bio = BIO_new_mem_buf(pub_pem.data(), (int)pub_pem.size());
  if (!bio) return false;
  EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!pkey) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = false;
  if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
      EVP_DigestVerify(ctx, (const unsigned char*)sig.data(), sig.size(),
                       (const unsigned char*)msg.data(), msg.size()) == 1) {
    ok = true;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(pkey);
  return ok;
}

}  // namespace manifest
}  // namespace tinyborg
