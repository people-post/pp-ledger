#include "Utilities.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

TEST(Sha256Test, Sha256RawMatchesHexEncoding) {
  std::string raw = sha256Raw("hello world");
  EXPECT_EQ(raw.size(), SHA256_DIGEST_SIZE);
  EXPECT_EQ(hexEncode(raw), sha256("hello world"));
  EXPECT_EQ(zeroHash().size(), SHA256_DIGEST_SIZE);
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

// --- readPrivateKey ---

namespace {

/** A raw key with chosen edge bytes; the middle is a fixed non-whitespace byte. */
std::string rawKeyWithEdges(const std::string &head, const std::string &tail) {
  std::string key(kMlDsaPrivateKeyBytes, '\x5a');
  key.replace(0, head.size(), head);
  key.replace(key.size() - tail.size(), tail.size(), tail);
  return key;
}

std::filesystem::path writeKeyFile(const std::string &name,
                                   const std::string &bytes) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

} // namespace

// Regression: raw keys were whitespace-trimmed / 0x-stripped as if they were
// text, so a random key starting or ending with such bytes failed to load.
TEST(ReadPrivateKeyTest, RawKeyWithWhitespaceOrHexPrefixEdgesLoadsIntact) {
  const std::vector<std::pair<std::string, std::string>> edges = {
      {" ", "x"}, {"x", "\n"}, {"\t\r", " \n"}, {"0x", "y"}, {"\x1a", "z"}};
  for (const auto &[head, tail] : edges) {
    const std::string key = rawKeyWithEdges(head, tail);
    const auto path = writeKeyFile("pp-ledger-raw-key-test.bin", key);
    auto result = readPrivateKey(path.string(), "");
    ASSERT_TRUE(result.isOk()) << result.error().message;
    EXPECT_EQ(result.value(), key);
    std::filesystem::remove(path);
  }
}

TEST(ReadPrivateKeyTest, HexKeyFileWithPrefixAndNewlineLoads) {
  const std::string key = rawKeyWithEdges(" ", "\n");
  const auto path =
      writeKeyFile("pp-ledger-hex-key-test.txt", "0x" + hexEncode(key) + "\n");
  auto result = readPrivateKey(path.string(), "");
  ASSERT_TRUE(result.isOk()) << result.error().message;
  EXPECT_EQ(result.value(), key);
  std::filesystem::remove(path);
}

TEST(ReadPrivateKeyTest, RelativePathResolvesAgainstBaseDir) {
  const std::string key = rawKeyWithEdges("a", "b");
  const auto path = writeKeyFile("pp-ledger-relative-key-test.bin", key);
  auto result = readPrivateKey(path.filename().string(),
                               path.parent_path().string());
  ASSERT_TRUE(result.isOk()) << result.error().message;
  EXPECT_EQ(result.value(), key);
  std::filesystem::remove(path);
}

TEST(ReadPrivateKeyTest, InlineHexKeyLoads) {
  const std::string key = rawKeyWithEdges("a", "b");
  auto result = readPrivateKey(" " + hexEncode(key) + " ", "");
  ASSERT_TRUE(result.isOk()) << result.error().message;
  EXPECT_EQ(result.value(), key);
}

TEST(ReadPrivateKeyTest, WrongSizeRawFileIsRejected) {
  const auto path = writeKeyFile("pp-ledger-short-key-test.bin",
                                 std::string(kMlDsaPrivateKeyBytes - 1, '\x5a'));
  EXPECT_TRUE(readPrivateKey(path.string(), "").isError());
  std::filesystem::remove(path);
}

// --- writeToNewFile ---

TEST(WriteToNewFileTest, WritesOwnerOnlyPermissions) {
#if defined(_WIN32)
  GTEST_SKIP() << "0600 is POSIX-only; std::filesystem has no group/others on Windows";
#endif
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               "pp-ledger-write-to-new-file-perms-test.txt";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  auto result = writeToNewFile(path.string(), "secret content\n");
  ASSERT_TRUE(result.isOk()) << result.error().message;

  auto perms = std::filesystem::status(path, ec).permissions();
  ASSERT_FALSE(ec);
  const auto forbidden = std::filesystem::perms::group_read |
                        std::filesystem::perms::group_write |
                        std::filesystem::perms::group_exec |
                        std::filesystem::perms::others_read |
                        std::filesystem::perms::others_write |
                        std::filesystem::perms::others_exec;
  EXPECT_EQ(perms & forbidden, std::filesystem::perms::none);
  EXPECT_NE(perms & std::filesystem::perms::owner_read,
           std::filesystem::perms::none);
  EXPECT_NE(perms & std::filesystem::perms::owner_write,
           std::filesystem::perms::none);

  std::filesystem::remove(path, ec);
}

TEST(WriteToNewFileTest, NeverOverwritesExistingFile) {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               "pp-ledger-write-to-new-file-exists-test.txt";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(writeToNewFile(path.string(), "first\n").isOk());
  auto second = writeToNewFile(path.string(), "second\n");
  EXPECT_FALSE(second.isOk());

  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "first");

  std::filesystem::remove(path, ec);
}

}  // namespace utl
}  // namespace pp

// A path that does not exist is reported as a missing file, not as a
// malformed inline key.
TEST(ReadPrivateKeyTest, MissingKeyFileIsReportedAsNotFound) {
  auto key = pp::utl::readPrivateKey("keys/missing.key", "/nonexistent-dir");
  ASSERT_FALSE(key.isOk());
  EXPECT_NE(key.error().message.find("Key file not found"), std::string::npos) << key.error().message;
}
