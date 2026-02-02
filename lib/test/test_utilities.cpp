#include "Utilities.h"
#include <gtest/gtest.h>

namespace pp {
namespace utl {

TEST(Ed25519Test, GenerateReturnsValidKeyPair) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk()) << (pair.isError() ? pair.error().message : "");
  EXPECT_EQ(pair->publicKey.size(), 32u);
  EXPECT_EQ(pair->privateKey.size(), 32u);
}

TEST(Ed25519Test, SignReturns64ByteSignature) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "hello world";
  auto sig = ed25519Sign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk()) << (sig.isError() ? sig.error().message : "");
  EXPECT_EQ(sig->size(), 64u);
}

TEST(Ed25519Test, VerifyValidSignatureReturnsTrue) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "test message";
  auto sig = ed25519Sign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_TRUE(ed25519Verify(pair->publicKey, message, *sig));
}

TEST(Ed25519Test, VerifyWrongMessageReturnsFalse) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "original";
  auto sig = ed25519Sign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_FALSE(ed25519Verify(pair->publicKey, "tampered", *sig));
}

TEST(Ed25519Test, VerifyWrongSignatureReturnsFalse) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "message";
  std::string wrongSig(64, '\0');  // all zeros is not a valid signature
  EXPECT_FALSE(ed25519Verify(pair->publicKey, message, wrongSig));
}

TEST(Ed25519Test, VerifyWrongPublicKeyReturnsFalse) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "message";
  auto sig = ed25519Sign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  std::string wrongPub(32, '\x01');
  EXPECT_FALSE(ed25519Verify(wrongPub, message, *sig));
}

TEST(Ed25519Test, SignWithWrongPrivateKeySizeReturnsError) {
  std::string shortKey(16, '\0');
  auto sig = ed25519Sign(shortKey, "msg");
  EXPECT_TRUE(sig.isError());
  EXPECT_EQ(sig.error().code, 1);
}

TEST(Ed25519Test, RoundTripGenerateSignVerify) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string message = "round-trip payload";
  auto sig = ed25519Sign(pair->privateKey, message);
  ASSERT_TRUE(sig.isOk());
  EXPECT_TRUE(ed25519Verify(pair->publicKey, message, *sig));
}

TEST(Ed25519Test, DifferentKeysProduceDifferentSignatures) {
  auto pair1 = ed25519Generate();
  auto pair2 = ed25519Generate();
  ASSERT_TRUE(pair1.isOk() && pair2.isOk());
  EXPECT_NE(pair1->publicKey, pair2->publicKey);
  EXPECT_NE(pair1->privateKey, pair2->privateKey);
  std::string message = "same message";
  auto sig1 = ed25519Sign(pair1->privateKey, message);
  auto sig2 = ed25519Sign(pair2->privateKey, message);
  ASSERT_TRUE(sig1.isOk() && sig2.isOk());
  EXPECT_NE(*sig1, *sig2);
  EXPECT_TRUE(ed25519Verify(pair1->publicKey, message, *sig1));
  EXPECT_TRUE(ed25519Verify(pair2->publicKey, message, *sig2));
}

TEST(Ed25519Test, VerifyRejectsWrongSignatureSize) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string shortSig(32, '\0');
  EXPECT_FALSE(ed25519Verify(pair->publicKey, "msg", shortSig));
  std::string longSig(128, '\0');
  EXPECT_FALSE(ed25519Verify(pair->publicKey, "msg", longSig));
}

TEST(Ed25519Test, EmptyMessageSignAndVerify) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string empty;
  auto sig = ed25519Sign(pair->privateKey, empty);
  ASSERT_TRUE(sig.isOk());
  EXPECT_EQ(sig->size(), 64u);
  EXPECT_TRUE(ed25519Verify(pair->publicKey, empty, *sig));
}

}  // namespace utl
}  // namespace pp
