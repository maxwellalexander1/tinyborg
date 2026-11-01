#include <gtest/gtest.h>
#include "../../common/manifest.h"
#include <cstdlib>

// Generate a throwaway Ed25519 keypair via openssl CLI for tests.
static bool GenKeypair(std::string* priv_pem, std::string* pub_pem) {
  char priv_tpl[] = "/tmp/mtest_privXXXXXX";
  char pub_tpl[] = "/tmp/mtest_pubXXXXXX";
  int fd1 = mkstemp(priv_tpl);
  int fd2 = mkstemp(pub_tpl);
  if (fd1 < 0 || fd2 < 0) return false;
  close(fd1);
  close(fd2);
  std::string cmd1 = std::string("openssl genpkey -algorithm Ed25519 -out ") + priv_tpl + " 2>/dev/null";
  std::string cmd2 = std::string("openssl pkey -in ") + priv_tpl + " -pubout -out " + pub_tpl + " 2>/dev/null";
  if (system(cmd1.c_str()) != 0) return false;
  if (system(cmd2.c_str()) != 0) return false;
  auto read = [](const std::string& p) {
    FILE* f = fopen(p.c_str(), "r");
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
  };
  *priv_pem = read(priv_tpl);
  *pub_pem = read(pub_tpl);
  remove(priv_tpl);
  remove(pub_tpl);
  return !priv_pem->empty() && !pub_pem->empty();
}

TEST(Manifest, SignVerifyRoundTrip) {
  std::string priv, pub;
  ASSERT_TRUE(GenKeypair(&priv, &pub));
  std::string msg = tinyborg::manifest::Canonical("j1", "/bin/sleep", "60", 1, 256, 0, 1000, 1300, "default");
  std::string sig;
  ASSERT_TRUE(tinyborg::manifest::Sign(priv, msg, &sig));
  EXPECT_TRUE(tinyborg::manifest::Verify(pub, msg, sig));
}

TEST(Manifest, TamperedFieldFails) {
  std::string priv, pub;
  ASSERT_TRUE(GenKeypair(&priv, &pub));
  std::string msg = tinyborg::manifest::Canonical("j1", "/bin/sleep", "60", 1, 256, 0, 1000, 1300, "default");
  std::string sig;
  ASSERT_TRUE(tinyborg::manifest::Sign(priv, msg, &sig));
  std::string evil = tinyborg::manifest::Canonical("j1", "/bin/sleep", "600", 1, 256, 0, 1000, 1300, "default");
  EXPECT_FALSE(tinyborg::manifest::Verify(pub, evil, sig));
}

TEST(Manifest, WrongKeyFails) {
  std::string priv1, pub1, priv2, pub2;
  ASSERT_TRUE(GenKeypair(&priv1, &pub1));
  ASSERT_TRUE(GenKeypair(&priv2, &pub2));
  std::string msg = tinyborg::manifest::Canonical("j", "/bin/true", "", 1, 256, 0, 1000, 1300, "default");
  std::string sig;
  ASSERT_TRUE(tinyborg::manifest::Sign(priv1, msg, &sig));
  EXPECT_FALSE(tinyborg::manifest::Verify(pub2, msg, sig));
}

TEST(Manifest, EmptySigFails) {
  std::string priv, pub;
  ASSERT_TRUE(GenKeypair(&priv, &pub));
  std::string msg = tinyborg::manifest::Canonical("j", "/bin/true", "", 1, 256, 0, 1000, 1300, "default");
  EXPECT_FALSE(tinyborg::manifest::Verify(pub, msg, ""));
}
