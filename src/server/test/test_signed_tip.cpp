#include "ledger/server/TipSigner.h"
#include "ledger/server/UpstreamSignedTip.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include "ledger/testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace pp;

// beacon-authority ba3: the beacon signs each committed block and keeps only the latest signed
// tip; relays hold the newest one they hear and pass it on unchanged.

namespace {

class TipSignerTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = pp::test::uniqueTempPath("pp-ledger-tip-signer-" +
                                    std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_);
    key_ = utl::mlDsaGenerate().value();
    cert_.networkId = "test-net";
    cert_.generation = 1;
    cert_.publicKey = key_.publicKey;
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string path() const { return (dir_ / "signed-tip.dat").string(); }

  std::filesystem::path dir_;
  utl::MlDsaKeyPair key_;
  BeaconCertificate cert_;
};

Client::SignedTip tipAt(uint64_t height, uint64_t generation = 1, const std::string &hash = "") {
  Client::SignedTip tip;
  tip.networkId = "test-net";
  tip.generation = generation;
  tip.height = height;
  tip.blockHash = hash.empty() ? "hash-" + std::to_string(height) : hash;
  tip.signature = "sig";
  return tip;
}

} // namespace

TEST_F(TipSignerTest, SignsWithTheCertifiedKeyAndPersistsTheLatest) {
  TipSigner signer(path(), {key_});
  ASSERT_TRUE(signer.load().isOk());
  EXPECT_FALSE(signer.latest().has_value()) << "a fresh beacon has signed nothing";

  ASSERT_TRUE(signer.sign(cert_, 0, "hash-0").isOk());
  ASSERT_TRUE(signer.sign(cert_, 1, "hash-1").isOk());
  const auto tip = signer.latest().value();
  EXPECT_EQ(tip.networkId, "test-net");
  EXPECT_EQ(tip.generation, 1u);
  EXPECT_EQ(tip.height, 1u);
  EXPECT_EQ(tip.blockHash, "hash-1");
  EXPECT_TRUE(tip.verify(cert_.publicKey));
  EXPECT_FALSE(tip.verify(utl::mlDsaGenerate().value().publicKey));

  auto forged = tip;
  forged.height = 2; // a signature covers its height, hash, generation and network
  EXPECT_FALSE(forged.verify(cert_.publicKey));

  TipSigner reopened(path(), {key_});
  ASSERT_TRUE(reopened.load().isOk());
  EXPECT_EQ(reopened.latest(), tip) << "only the latest tip is kept, and it survives a restart";
}

TEST_F(TipSignerTest, NeverSignsASecondBlockAtOrBelowTheLastSignedHeight) {
  TipSigner signer(path(), {key_});
  ASSERT_TRUE(signer.load().isOk());
  ASSERT_TRUE(signer.sign(cert_, 5, "hash-5").isOk());
  const auto signedTip = signer.latest().value();

  EXPECT_TRUE(signer.sign(cert_, 5, "hash-5").isOk()) << "the same block again is a no-op";
  EXPECT_EQ(signer.latest(), signedTip) << "not signed again";

  auto beside = signer.sign(cert_, 5, "other-hash");
  ASSERT_FALSE(beside.isOk());
  EXPECT_EQ(beside.error().code, TipSigner::E_CONFLICT);
  auto below = signer.sign(cert_, 4, "hash-4");
  ASSERT_FALSE(below.isOk());
  EXPECT_EQ(below.error().code, TipSigner::E_CONFLICT);
  EXPECT_EQ(signer.latest(), signedTip);

  TipSigner restarted(path(), {key_}); // the rule holds across a restart
  ASSERT_TRUE(restarted.load().isOk());
  EXPECT_EQ(restarted.sign(cert_, 5, "other-hash").error().code, TipSigner::E_CONFLICT);
}

TEST_F(TipSignerTest, DoesNotSignWhenTheCertificateNamesAnotherKey) {
  TipSigner signer(path(), {key_});
  ASSERT_TRUE(signer.load().isOk());
  BeaconCertificate other = cert_;
  other.generation = 2;
  other.publicKey = utl::mlDsaGenerate().value().publicKey;
  auto refused = signer.sign(other, 0, "hash-0");
  ASSERT_FALSE(refused.isOk());
  EXPECT_EQ(refused.error().code, TipSigner::E_NOT_CERTIFIED);
  EXPECT_FALSE(signer.latest().has_value());
  EXPECT_FALSE(std::filesystem::exists(path()));
}

// beacon-authority ba5: a beacon holding the next key switches to it once a certificate names it.
TEST_F(TipSignerTest, SignsWithWhicheverHeldKeyTheCertificateNames) {
  const auto next = utl::mlDsaGenerate().value();
  TipSigner signer(path(), {key_, next});
  ASSERT_TRUE(signer.load().isOk());
  EXPECT_TRUE(signer.holdsKey(next.publicKey));
  ASSERT_TRUE(signer.sign(cert_, 3, "hash-3").isOk());
  EXPECT_TRUE(signer.latest()->verify(key_.publicKey));

  BeaconCertificate rotated = cert_;
  rotated.generation = 2;
  rotated.publicKey = next.publicKey;
  ASSERT_TRUE(signer.sign(rotated, 4, "hash-4").isOk());
  EXPECT_EQ(signer.latest()->generation, 2u);
  EXPECT_TRUE(signer.latest()->verify(next.publicKey));
  EXPECT_FALSE(signer.holdsKey(utl::mlDsaGenerate().value().publicKey));
}

TEST_F(TipSignerTest, AFileThatIsNotASignedTipIsRefused) {
  std::ofstream(path()) << "garbage";
  TipSigner signer(path(), {key_});
  EXPECT_FALSE(signer.load().isOk());
}

TEST(SignedTipWireTest, TravelsInStatusAndTipReplies) {
  Client::BeaconState state;
  state.nextBlockId = 8;
  state.signedTip = tipAt(7);
  Client::BeaconState parsed;
  ASSERT_TRUE(parsed.ltsFromMeta(state.ltsToMeta()).isOk());
  EXPECT_EQ(parsed.signedTip, state.signedTip);

  state.signedTip.reset();
  ASSERT_TRUE(parsed.ltsFromMeta(state.ltsToMeta()).isOk());
  EXPECT_FALSE(parsed.signedTip.has_value());
  EXPECT_FALSE(parsed.certificate.has_value());

  // beacon-authority ba5c: the current certificate rides STATUS as its record.
  Ledger::Record cert;
  cert.type = Ledger::T_BEACON_CERT;
  cert.data = "certificate";
  cert.signatures = {"admin"};
  state.certificate = cert;
  ASSERT_TRUE(parsed.ltsFromMeta(state.ltsToMeta()).isOk());
  ASSERT_TRUE(parsed.certificate.has_value());
  EXPECT_EQ(parsed.certificate->data, "certificate");
  EXPECT_EQ(parsed.certificate->signatures, cert.signatures);
  state.certificate->type = Ledger::T_DEFAULT;
  EXPECT_FALSE(parsed.ltsFromMeta(state.ltsToMeta()).isOk()) << "only a beacon certificate";
  state.certificate.reset();

  // beacon-authority ba6: equivocation evidence rides STATUS too.
  EXPECT_FALSE(parsed.equivocation.has_value());
  state.equivocation = Client::Equivocation::of(tipAt(7), tipAt(7, 1, "another block"));
  ASSERT_TRUE(state.equivocation.has_value());
  ASSERT_TRUE(parsed.ltsFromMeta(state.ltsToMeta()).isOk());
  EXPECT_EQ(parsed.equivocation, state.equivocation);

  // beacon-authority ba7: a pending recovery rides STATUS too.
  EXPECT_FALSE(parsed.recovery.has_value());
  state.recovery = Client::RecoveryStatus{12, 40, {"key-a", "key-b", "key-c"}, 2};
  ASSERT_TRUE(parsed.ltsFromMeta(state.ltsToMeta()).isOk());
  EXPECT_EQ(parsed.recovery, state.recovery);

  Client::TipReply reply;
  reply.nextBlockId = 8;
  reply.setTip(tipAt(7));
  auto unpacked = utl::binaryUnpack<Client::TipReply>(utl::binaryPack(reply));
  ASSERT_TRUE(unpacked.isOk());
  EXPECT_EQ(unpacked.value().nextBlockId, 8u);
  EXPECT_EQ(unpacked.value().tip(), tipAt(7));
}

TEST(UpstreamSignedTipTest, KeepsTheNewestAndIgnoresOlderOrConflicting) {
  UpstreamSignedTip held;
  EXPECT_FALSE(held.latest().has_value());
  EXPECT_EQ(held.offer(tipAt(5)), UpstreamSignedTip::Offer::Newer);
  EXPECT_EQ(held.offer(tipAt(5)), UpstreamSignedTip::Offer::Same);
  EXPECT_EQ(held.offer(tipAt(4)), UpstreamSignedTip::Offer::Older);
  EXPECT_EQ(held.offer(tipAt(5, 1, "other")), UpstreamSignedTip::Offer::Conflict) << "another block";
  auto resigned = tipAt(5);
  resigned.signature = "another signature";
  EXPECT_EQ(held.offer(resigned), UpstreamSignedTip::Offer::Same) << "one block signed twice is no conflict";
  EXPECT_EQ(held.latest(), tipAt(5));

  EXPECT_EQ(held.offer(tipAt(5, 2, "other")), UpstreamSignedTip::Offer::Newer) << "a higher generation at one height";
  EXPECT_EQ(held.offer(tipAt(5, 1)), UpstreamSignedTip::Offer::Older);
  EXPECT_EQ(held.offer(tipAt(6, 2)), UpstreamSignedTip::Offer::Newer);
  EXPECT_EQ(held.latest(), tipAt(6, 2));
}
