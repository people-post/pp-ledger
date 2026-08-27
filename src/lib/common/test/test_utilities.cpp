#include "Utilities.h"
#include <gtest/gtest.h>

namespace pp {
namespace utl {

// SHA-256 tests
TEST(Sha256Test, EmptyStringProducesKnownHash) {
  std::string hash = sha256("");
  // SHA-256 of empty string
  EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, HelloWorldProducesKnownHash) {
  std::string hash = sha256("hello world");
  // SHA-256 of "hello world"
  EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST(Sha256Test, DifferentInputsProduceDifferentHashes) {
  std::string hash1 = sha256("test1");
  std::string hash2 = sha256("test2");
  EXPECT_NE(hash1, hash2);
}

TEST(Sha256Test, SameInputProducesSameHash) {
  std::string input = "consistent input";
  std::string hash1 = sha256(input);
  std::string hash2 = sha256(input);
  EXPECT_EQ(hash1, hash2);
}

TEST(Sha256Test, OutputIsHexadecimal64Characters) {
  std::string hash = sha256("test");
  EXPECT_EQ(hash.size(), 64u);  // SHA-256 produces 32 bytes = 64 hex chars
  // Verify all characters are valid hex
  for (char c : hash) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

// ML-DSA-65 tests
TEST(MlDsaTest, GenerateReturnsValidKeyPair) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk()) << (pair.isError() ? pair.error().message : "");
  EXPECT_EQ(pair->publicKey.size(), kMlDsaPublicKeyBytes);
  EXPECT_EQ(pair->privateKey.size(), kMlDsaPrivateKeyBytes);
}

TEST(MlDsaTest, SignReturnsExpectedSignatureSize) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "hello world";
  auto sig = mlDsaSign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk()) << (sig.isError() ? sig.error().message : "");
  EXPECT_EQ(sig->size(), kMlDsaSignatureBytes);
}

TEST(MlDsaTest, VerifyValidSignatureReturnsTrue) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "test message";
  auto sig = mlDsaSign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_TRUE(mlDsaVerify(pair->publicKey, message, *sig));
}

TEST(MlDsaTest, VerifyWrongMessageReturnsFalse) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "original";
  auto sig = mlDsaSign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_FALSE(mlDsaVerify(pair->publicKey, "tampered", *sig));
}

TEST(MlDsaTest, VerifyWrongSignatureReturnsFalse) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "message";
  std::string wrongSig(kMlDsaSignatureBytes, '\0');
  EXPECT_FALSE(mlDsaVerify(pair->publicKey, message, wrongSig));
}

TEST(MlDsaTest, VerifyWrongPublicKeyReturnsFalse) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "message";
  auto sig = mlDsaSign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  auto other = mlDsaGenerate();
  ASSERT_TRUE(other.isOk());
  EXPECT_FALSE(mlDsaVerify(other->publicKey, message, *sig));
}

TEST(MlDsaTest, SignWithWrongPrivateKeySizeReturnsError) {
  std::string shortKey(16, '\0');
  auto sig = mlDsaSign(shortKey, "msg");
  EXPECT_TRUE(sig.isError());
  EXPECT_EQ(sig.error().code, 1);
}

TEST(MlDsaTest, RoundTripGenerateSignVerify) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "round-trip payload";
  auto sig = mlDsaSign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_TRUE(mlDsaVerify(pair->publicKey, message, *sig));
}

TEST(MlDsaTest, DifferentKeysProduceDifferentSignatures) {
  auto pair1 = mlDsaGenerate();
  auto pair2 = mlDsaGenerate();
  ASSERT_TRUE(pair1.isOk() && pair2.isOk());
  EXPECT_NE(pair1->publicKey, pair2->publicKey);
  EXPECT_NE(pair1->privateKey, pair2->privateKey);
  std::string message = "same message";
  auto sig1 = mlDsaSign(pair1->privateKey, message);
  auto sig2 = mlDsaSign(pair2->privateKey, message);
  ASSERT_TRUE(sig1.isOk() && sig2.isOk());
  EXPECT_NE(*sig1, *sig2);
  EXPECT_TRUE(mlDsaVerify(pair1->publicKey, message, *sig1));
  EXPECT_TRUE(mlDsaVerify(pair2->publicKey, message, *sig2));
}

TEST(MlDsaTest, VerifyRejectsWrongSignatureSize) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string shortSig(32, '\0');
  EXPECT_FALSE(mlDsaVerify(pair->publicKey, "msg", shortSig));
  std::string longSig(kMlDsaSignatureBytes + 64, '\0');
  EXPECT_FALSE(mlDsaVerify(pair->publicKey, "msg", longSig));
}

TEST(MlDsaTest, EmptyMessageSignAndVerify) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string empty;
  auto sig = mlDsaSign(pair->privateKey, empty);
  ASSERT_TRUE(sig.isOk());
  EXPECT_EQ(sig->size(), kMlDsaSignatureBytes);
  EXPECT_TRUE(mlDsaVerify(pair->publicKey, empty, *sig));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyRaw) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  EXPECT_TRUE(isValidMlDsaPublicKey(pair->publicKey));
  EXPECT_TRUE(isValidPublicKey(pair->publicKey));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyHex) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  std::string hexPub = hexEncode(pair->publicKey);
  EXPECT_EQ(hexPub.size(), kMlDsaPublicKeyBytes * 2);
  EXPECT_TRUE(isValidMlDsaPublicKey(hexPub));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyHex0xPrefix) {
  auto pair = mlDsaGenerate();
  ASSERT_TRUE(pair.isOk());
  EXPECT_TRUE(isValidMlDsaPublicKey("0x" + hexEncode(pair->publicKey)));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyRejectsWrongLength) {
  EXPECT_FALSE(isValidMlDsaPublicKey(""));
  EXPECT_FALSE(isValidMlDsaPublicKey("short"));
  EXPECT_FALSE(isValidMlDsaPublicKey(std::string(kMlDsaPublicKeyBytes - 1, '\0')));
  EXPECT_FALSE(isValidMlDsaPublicKey(std::string(kMlDsaPublicKeyBytes + 1, '\0')));
  EXPECT_FALSE(isValidMlDsaPublicKey(std::string(64, 'a')));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyRejectsAllZero) {
  EXPECT_FALSE(isValidMlDsaPublicKey(std::string(kMlDsaPublicKeyBytes, '\0')));
}

TEST(MlDsaTest, IsValidMlDsaPublicKeyRejectsInvalidHex) {
  EXPECT_FALSE(isValidMlDsaPublicKey("0xgg" + std::string(kMlDsaPublicKeyBytes * 2 - 2, 'a')));
}

}  // namespace utl
}  // namespace pp
