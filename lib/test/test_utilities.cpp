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

// Ed25519 tests
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

TEST(Ed25519Test, IsValidEd25519PublicKeyRaw32Bytes) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  EXPECT_TRUE(isValidEd25519PublicKey(pair->publicKey));
  EXPECT_TRUE(isValidPublicKey(pair->publicKey));
}

TEST(Ed25519Test, IsValidEd25519PublicKeyHex64) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  std::string hexPub = hexEncode(pair->publicKey);
  EXPECT_EQ(hexPub.size(), 64u);
  EXPECT_TRUE(isValidEd25519PublicKey(hexPub));
}

TEST(Ed25519Test, IsValidEd25519PublicKeyHex0xPrefix) {
  auto pair = ed25519Generate();
  ASSERT_TRUE(pair.isOk());
  EXPECT_TRUE(isValidEd25519PublicKey("0x" + hexEncode(pair->publicKey)));
}

TEST(Ed25519Test, IsValidEd25519PublicKeyRejectsWrongLength) {
  EXPECT_FALSE(isValidEd25519PublicKey(""));
  EXPECT_FALSE(isValidEd25519PublicKey("short"));
  EXPECT_FALSE(isValidEd25519PublicKey(std::string(31, '\0')));
  EXPECT_FALSE(isValidEd25519PublicKey(std::string(33, '\0')));
  EXPECT_FALSE(isValidEd25519PublicKey(std::string(63, 'a')));
  EXPECT_FALSE(isValidEd25519PublicKey(std::string(65, 'a')));
}

TEST(Ed25519Test, IsValidEd25519PublicKeyRejectsInvalidHex) {
  EXPECT_FALSE(isValidEd25519PublicKey("0xgg" + std::string(60, 'a')));
}

}  // namespace utl
}  // namespace pp
