// Operation files (docs/contracts/OPERATION_FILE.md): built, written and read back, signed apart and
// merged, refused when tampered with, summarized from the record alone.
#include "../Operation.h"

#include "../../chain/Types.h"
#include "../../client/AccountIds.h"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

using namespace pp;
using namespace pp::operation;

namespace {

struct Keys {
  std::vector<utl::MlDsaKeyPair> pairs;
  explicit Keys(size_t n) {
    for (size_t i = 0; i < n; ++i) {
      pairs.push_back(utl::mlDsaGenerate().value());
    }
  }
  std::vector<std::string> publicKeys() const {
    std::vector<std::string> out;
    for (const auto &p : pairs) {
      out.push_back(p.publicKey);
    }
    return out;
  }
};

Common common(const Keys &keys, uint32_t minSignatures, std::optional<uint64_t> account) {
  Common c;
  c.networkId = "test-net";
  c.genesisHash = std::string(32, '\x07');
  c.signers.account = account;
  c.signers.minSignatures = minSignatures;
  c.signers.publicKeys = keys.publicKeys();
  c.window = windowFrom(1'800'000'000, 86400);
  c.notes = "monthly treasury top-up";
  c.createdAt = 1'800'000'000;
  return c;
}

TEST(OperationTest, AFileReadsBackAsItWasWritten) {
  Keys keys(3);
  auto op = transfer(common(keys, 2, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 5'000'000, 10);
  ASSERT_TRUE(op) << op.error().message;
  ASSERT_TRUE(sign(op.value(), keys.pairs[1].privateKey));
  auto back = fromJson(toJson(op.value()));
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(back.value().kind, Kind::Transfer);
  EXPECT_EQ(back.value().networkId, "test-net");
  EXPECT_EQ(back.value().genesisHash, std::string(32, '\x07'));
  EXPECT_EQ(back.value().record.data, op.value().record.data);
  EXPECT_EQ(back.value().signers, op.value().signers);
  EXPECT_EQ(back.value().signatures, op.value().signatures);
  EXPECT_EQ(back.value().notes, "monthly treasury top-up");
  EXPECT_EQ(check(back.value()).signedBy.size(), 1u);
}

// AD002: signers sign their own copies, in parallel; any copy merges them.
TEST(OperationTest, CopiesSignedApartMergeIntoACompleteOperation) {
  Keys keys(3);
  auto op = mint(common(keys, 2, std::nullopt), 1'000'000, 0);
  ASSERT_TRUE(op) << op.error().message;
  EXPECT_EQ(op.value().kind, Kind::Mint);
  Operation alice = op.value(), carol = op.value();
  ASSERT_TRUE(sign(alice, keys.pairs[0].privateKey));
  ASSERT_TRUE(sign(carol, keys.pairs[2].privateKey));
  EXPECT_FALSE(signedRecord(alice)) << "one of two: not yet";
  auto merged = merge({alice, carol, alice});
  ASSERT_TRUE(merged) << merged.error().message;
  const auto checked = check(merged.value());
  EXPECT_TRUE(checked.complete());
  EXPECT_EQ(checked.signedBy.size(), 2u);
  EXPECT_EQ(merged.value().signatures.size(), 2u) << "a key counted once";
  auto record = signedRecord(merged.value());
  ASSERT_TRUE(record) << record.error().message;
  for (const auto &signature : record.value().signatures) { // as the chain checks them
    bool byASigner = false;
    for (const auto &key : keys.publicKeys()) {
      byASigner = byASigner || utl::mlDsaVerify(key, record.value().signingMessage("test-net"), signature);
    }
    EXPECT_TRUE(byASigner);
  }
}

TEST(OperationTest, OnlyTheSignersSignAndEachOnce) {
  Keys keys(2);
  Keys stranger(1);
  auto op = transfer(common(keys, 2, AccountIds::ID_RESERVE), 1 << 30, 7, 1);
  ASSERT_TRUE(op);
  EXPECT_FALSE(sign(op.value(), stranger.pairs[0].privateKey));
  ASSERT_TRUE(sign(op.value(), keys.pairs[0].privateKey));
  EXPECT_FALSE(sign(op.value(), keys.pairs[0].privateKey)) << "a second signature by one key";
  EXPECT_EQ(op.value().signatures.size(), 1u);
}

// AD007: a file changed after signing shows: its signatures no longer hold; a claimed kind must
// be the record's.
TEST(OperationTest, ATamperedFileLosesItsSignatures) {
  Keys keys(1);
  auto op = transfer(common(keys, 1, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 100, 1);
  ASSERT_TRUE(op);
  ASSERT_TRUE(sign(op.value(), keys.pairs[0].privateKey));
  ASSERT_TRUE(check(op.value()).complete());

  auto changed = transfer(common(keys, 1, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 100'000, 1);
  ASSERT_TRUE(changed);
  changed.value().signatures = op.value().signatures; // the old signature on a new amount
  const auto checked = check(changed.value());
  EXPECT_FALSE(checked.complete());
  EXPECT_EQ(checked.invalid, 1u);

  std::string json = toJson(op.value());
  json.replace(json.find("\"transfer\""), 10, "\"mint\"");
  EXPECT_FALSE(fromJson(json)) << "the file says mint, the record is a transfer";
}

TEST(OperationTest, OnlyCopiesOfOneOperationMerge) {
  Keys keys(2);
  auto a = transfer(common(keys, 2, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 100, 1);
  auto b = transfer(common(keys, 2, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 100, 1);
  ASSERT_TRUE(a && b);
  EXPECT_FALSE(merge({a.value(), b.value()})) << "same terms, another idempotent id: another transaction";
}

TEST(OperationTest, SummariesComeFromTheRecord) {
  Keys keys(3);
  auto mintOp = mint(common(keys, 2, std::nullopt), 1'234'567, 0);
  ASSERT_TRUE(mintOp);
  auto s = summarize(mintOp.value());
  ASSERT_TRUE(s) << s.error().message;
  EXPECT_EQ(s.value().title, "Mint new tokens to the reserve");
  bool amount = false;
  for (const auto &[label, value] : s.value().lines) {
    amount = amount || (label == "Amount" && value == "1,234,567");
  }
  EXPECT_TRUE(amount);
  EXPECT_EQ(s.value().validUntil - s.value().validFrom, 86400);

  UserAccount account;
  account.wallet.publicKeys = {keys.pairs[0].publicKey, keys.pairs[1].publicKey};
  account.wallet.minSignatures = 2;
  account.wallet.keyType = 1;
  auto created = newAccount(common(keys, 2, AccountIds::ID_REGISTRAR), (1ULL << 20) + 5, account, 1000, 50);
  ASSERT_TRUE(created);
  auto c = summarize(created.value());
  ASSERT_TRUE(c);
  EXPECT_EQ(c.value().title, "Create an account");
  EXPECT_EQ(c.value().lines.back().first, "Its keys");
  EXPECT_EQ(c.value().lines.back().second.rfind("2 of 2:", 0), 0u);

  RecoveryAnnouncement announcement;
  announcement.networkId = "test-net";
  announcement.adminPublicKeys = keys.publicKeys();
  announcement.adminMinSignatures = 2;
  Ledger::TxRecovery recoveryTx;
  recoveryTx.meta = announcement.ltsToString();
  Ledger::Record record;
  record.type = Ledger::T_RECOVERY;
  record.data = utl::binaryPack(recoveryTx);
  auto recovery = wrap(common(keys, 2, std::nullopt), record);
  ASSERT_TRUE(recovery);
  EXPECT_EQ(recovery.value().kind, Kind::Recovery);
  auto r = summarize(recovery.value());
  ASSERT_TRUE(r) << r.error().message;
  EXPECT_EQ(r.value().title, "Replace the admin keys (recovery)");
  EXPECT_EQ(r.value().lines.back().second.rfind("2 of 3:", 0), 0u);
}

TEST(OperationTest, AmountsAreGroupedByThousands) {
  EXPECT_EQ(groupDigits(0), "0");
  EXPECT_EQ(groupDigits(999), "999");
  EXPECT_EQ(groupDigits(1000), "1,000");
  EXPECT_EQ(groupDigits(1073741824), "1,073,741,824");
}

} // namespace
