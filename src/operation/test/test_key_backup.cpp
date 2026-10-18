// Key backups (docs/contracts/KEY_BACKUP.md): written and read back, refused when anything is off.
#include "../KeyBackup.h"

#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

using namespace pp;
using namespace pp::operation;

namespace {

constexpr const char *kPassphrase = "correct horse battery";

BackedUpKey key() {
  auto pair = utl::mlDsaGenerate().value();
  return {"savings key", pair.publicKey, pair.privateKey};
}

} // namespace

TEST(KeyBackupTest, AKeyReadsBackWithItsPassphraseOnly) {
  const BackedUpKey original = key();
  auto json = keyBackupToJson(original, kPassphrase, KeyBackupLimits::minimum());
  ASSERT_TRUE(json) << json.error().message;
  EXPECT_EQ(json.value().find(utl::hexEncode(original.privateKey)), std::string::npos) << "never in the clear";
  EXPECT_EQ(json.value().find("savings key"), std::string::npos) << "the label is private too";
  EXPECT_EQ(keyBackupPublicKey(json.value()).value(), original.publicKey);

  auto back = keyBackupFromJson(json.value(), kPassphrase);
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(back.value().label, original.label);
  EXPECT_EQ(back.value().publicKey, original.publicKey);
  EXPECT_EQ(back.value().privateKey, original.privateKey);
  EXPECT_FALSE(keyBackupFromJson(json.value(), "not the passphrase"));
}

TEST(KeyBackupTest, AnythingOffIsRefused) {
  const BackedUpKey original = key();
  EXPECT_FALSE(keyBackupToJson(original, "short", KeyBackupLimits::minimum())) << "a passphrase under 10 characters";
  const std::string json = keyBackupToJson(original, kPassphrase, KeyBackupLimits::minimum()).value();

  // Another public key named in the clear: the ciphertext is bound to the one it was made with.
  std::string swapped = json;
  const std::string hex = utl::hexEncode(original.publicKey);
  swapped.replace(swapped.find(hex), hex.size(), utl::hexEncode(utl::mlDsaGenerate().value().publicKey));
  EXPECT_FALSE(keyBackupFromJson(swapped, kPassphrase));

  // Costs a reader will not pay.
  std::string greedy = json;
  const auto at = greedy.find("\"memLimit\": ");
  ASSERT_NE(at, std::string::npos);
  const auto end = greedy.find_first_of(",\n}", at);
  greedy.replace(at, end - at, "\"memLimit\": 1099511627776");
  EXPECT_FALSE(keyBackupFromJson(greedy, kPassphrase));

  EXPECT_FALSE(keyBackupFromJson("{\"format\": \"pp-ledger-operation\"}", kPassphrase));
  EXPECT_FALSE(keyBackupPublicKey("not json"));

  // A private key that is not the public key's.
  BackedUpKey mismatched = original;
  mismatched.privateKey = utl::mlDsaGenerate().value().privateKey;
  auto odd = keyBackupToJson(mismatched, kPassphrase, KeyBackupLimits::minimum());
  ASSERT_TRUE(odd);
  EXPECT_FALSE(keyBackupFromJson(odd.value(), kPassphrase));
}
