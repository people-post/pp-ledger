// Operation files (docs/contracts/OPERATION_FILE.md): built, written and read back, signed apart and
// merged, refused when tampered with, summarized from the record alone.
#include "../Operation.h"

#include "../../chain/Types.h"
#include "../../client/AccountIds.h"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

#include <set>

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

TEST(OperationTest, TheSignersAreTheAccountTheChainChecks) {
  Keys keys(2);
  auto op = transfer(common(keys, 1, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 1, 0);
  ASSERT_TRUE(op);
  EXPECT_EQ(signerAccountOf(op.value().record), AccountIds::ID_RESERVE);

  // A file naming other signers than the payer is refused: their signatures could never count.
  Operation other = op.value();
  other.signers.account = AccountIds::ID_REGISTRAR;
  EXPECT_FALSE(fromJson(toJson(other)));

  // A record from elsewhere wraps only with its own signers: a transfer's payer, a recovery's key set.
  EXPECT_FALSE(wrap(common(keys, 1, AccountIds::ID_FEE), op.value().record));
  EXPECT_TRUE(wrap(common(keys, 1, AccountIds::ID_RESERVE), op.value().record));
  Ledger::TxRecovery recovery;
  Ledger::Record record;
  record.type = Ledger::T_RECOVERY;
  record.data = utl::binaryPack(recovery);
  EXPECT_FALSE(wrap(common(keys, 1, AccountIds::ID_GENESIS), record));
}

/** Block 0 by hand (the beacon builds it with chain_genesis::records): genesis meta, two accounts. */
std::vector<Ledger::Record> genesisRecords(const Keys &admins) {
  GenesisAccountMeta meta;
  meta.config.networkId = "test-net";
  meta.config.genesisTime = 1'800'000'000;
  meta.genesis.wallet.publicKeys = admins.publicKeys();
  meta.genesis.wallet.minSignatures = 2;
  Ledger::TxGenesis genesisTx;
  genesisTx.meta = meta.ltsToString();
  std::vector<Ledger::Record> records(1);
  records[0].type = Ledger::T_GENESIS;
  records[0].data = utl::binaryPack(genesisTx);
  for (uint64_t id : {AccountIds::ID_FEE, AccountIds::ID_RESERVE}) {
    UserAccount account;
    account.wallet.publicKeys = {admins.pairs[0].publicKey};
    account.wallet.minSignatures = 1;
    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountIds::ID_GENESIS;
    tx.toWalletId = id;
    tx.amount = id == AccountIds::ID_RESERVE ? 1'000'000 : 0;
    tx.meta = account.ltsToString();
    Ledger::Record record;
    record.type = Ledger::T_NEW_USER;
    record.data = utl::binaryPack(tx);
    records.push_back(record);
  }
  return records;
}

TEST(OperationTest, AGenesisIsSignedRecordByRecordAndMerged) {
  Keys admins(3);
  auto op = genesis(genesisRecords(admins), "launch", 1'800'000'000);
  ASSERT_TRUE(op) << op.error().message;
  EXPECT_EQ(op.value().kind, Kind::Genesis);
  EXPECT_EQ(op.value().networkId, "test-net");
  EXPECT_EQ(op.value().signers.account, AccountIds::ID_GENESIS);
  EXPECT_EQ(op.value().signers.minSignatures, 2u);
  ASSERT_EQ(op.value().moreRecords.size(), 2u);

  // Two admins sign their own copies: each copy carries a signature of every record.
  auto a = fromJson(toJson(op.value())).value();
  auto b = fromJson(toJson(op.value())).value();
  ASSERT_TRUE(sign(a, admins.pairs[0].privateKey));
  ASSERT_TRUE(sign(b, admins.pairs[2].privateKey));
  EXPECT_EQ(a.moreRecords[1].signatures.size(), 1u);
  EXPECT_FALSE(signedRecords(a));
  auto merged = merge({fromJson(toJson(a)).value(), fromJson(toJson(b)).value()});
  ASSERT_TRUE(merged) << merged.error().message;
  EXPECT_TRUE(check(merged.value()).complete());
  auto records = signedRecords(merged.value());
  ASSERT_TRUE(records) << records.error().message;
  ASSERT_EQ(records.value().size(), 3u);
  for (const auto &record : records.value()) {
    EXPECT_EQ(record.signatures.size(), 2u);
  }

  // A key that has signed some records only has not signed the operation.
  Operation partial = a;
  partial.moreRecords[1].signatures.clear();
  EXPECT_TRUE(check(partial).signedBy.empty());
  // A changed account record breaks its signatures, and copies with it no longer merge.
  Operation tampered = merged.value();
  auto tx = utl::binaryUnpack<Ledger::TxNewUser>(tampered.moreRecords[1].data).value();
  tx.amount += 1;
  tampered.moreRecords[1].data = utl::binaryPack(tx);
  auto reread = fromJson(toJson(tampered));
  ASSERT_TRUE(reread);
  EXPECT_FALSE(check(reread.value()).complete());
  EXPECT_FALSE(merge({merged.value(), reread.value()}));

  auto s = summarize(merged.value());
  ASSERT_TRUE(s) << s.error().message;
  EXPECT_EQ(s.value().title, "Start the chain (genesis)");
  bool reserve = false;
  for (const auto &[label, value] : s.value().lines) {
    reserve = reserve || (label == "Reserve" && value.rfind("1,000,000; keys 1 of 1:", 0) == 0);
  }
  EXPECT_TRUE(reserve);
}

TEST(OperationTest, MoreRecordsAreAGenesissAccountsOrABatchsTransfers) {
  Keys keys(2);
  auto op = transfer(common(keys, 1, AccountIds::ID_RESERVE), AccountIds::ID_REGISTRAR, 1, 0);
  ASSERT_TRUE(op);
  Operation withMore = op.value();
  withMore.moreRecords.push_back(genesisRecords(Keys(3))[1]);
  EXPECT_FALSE(fromJson(toJson(withMore)));
  // A batch's more records are transfers from its account, each once.
  auto other = transfer(common(keys, 1, AccountIds::ID_REGISTRAR), AccountIds::ID_RESERVE, 1, 0);
  ASSERT_TRUE(other);
  Operation fromAnother = op.value();
  fromAnother.moreRecords.push_back(other.value().record);
  EXPECT_FALSE(fromJson(toJson(fromAnother)));
  Operation twice = op.value();
  twice.moreRecords.push_back(op.value().record);
  EXPECT_FALSE(fromJson(toJson(twice)));
  auto mint = pp::operation::mint(common(keys, 1, AccountIds::ID_GENESIS), 5, 0);
  ASSERT_TRUE(mint);
  Operation mints = mint.value();
  auto again = pp::operation::mint(common(keys, 1, AccountIds::ID_GENESIS), 5, 0);
  mints.moreRecords.push_back(again.value().record);
  EXPECT_FALSE(fromJson(toJson(mints))); // a mint is not batched
  // Nor does a genesis carry anything but accounts the genesis account creates.
  Keys admins(3);
  auto records = genesisRecords(admins);
  records.push_back(op.value().record);
  EXPECT_FALSE(genesis(records, "", 0));
}

TEST(OperationTest, ABatchIsSignedOnceAndSubmittedTransferByTransfer) {
  Keys keys(3);
  const std::vector<Payment> payments = {{AccountIds::ID_FIRST_USER + 1, 700, 0},
                                         {AccountIds::ID_FIRST_ISSUED + 9, 300, 0},
                                         {AccountIds::ID_FIRST_USER + 2, 50, AccountIds::ID_FIRST_ISSUED + 5}};
  auto op = transfers(common(keys, 2, AccountIds::ID_FEE), payments, 2, "round 3");
  ASSERT_TRUE(op) << op.error().message;
  EXPECT_EQ(op.value().kind, Kind::Batch);
  ASSERT_EQ(op.value().moreRecords.size(), 2u);
  // Each its own transaction: distinct idempotency ids, one window, the same metadata.
  std::set<uint64_t> ids;
  for (const auto *record : {&op.value().record, &op.value().moreRecords[0], &op.value().moreRecords[1]}) {
    auto tx = utl::binaryUnpack<Ledger::TxDefault>(record->data);
    ASSERT_TRUE(tx);
    EXPECT_EQ(tx.value().fromWalletId, AccountIds::ID_FEE);
    EXPECT_EQ(tx.value().fee, 2u);
    EXPECT_EQ(tx.value().meta, "round 3");
    ids.insert(tx.value().idempotentId);
  }
  EXPECT_EQ(ids.size(), 3u);

  // Signed apart, merged: complete once two holders signed every transfer.
  Operation a = op.value();
  Operation b = op.value();
  ASSERT_TRUE(sign(a, keys.pairs[0].privateKey));
  ASSERT_TRUE(sign(b, keys.pairs[2].privateKey));
  EXPECT_FALSE(signedRecords(a));
  auto merged = merge({fromJson(toJson(a)).value(), fromJson(toJson(b)).value()});
  ASSERT_TRUE(merged) << merged.error().message;
  EXPECT_EQ(merged.value().kind, Kind::Batch);
  auto records = signedRecords(merged.value());
  ASSERT_TRUE(records) << records.error().message;
  ASSERT_EQ(records.value().size(), 3u);
  for (const auto &record : records.value()) {
    EXPECT_EQ(record.signatures.size(), 2u);
  }

  auto summary = summarize(merged.value());
  ASSERT_TRUE(summary) << summary.error().message;
  EXPECT_EQ(summary.value().title, "3 transfers from Fee");
  bool total = false;
  bool fees = false;
  for (const auto &[label, value] : summary.value().lines) {
    total = total || (label == "Total" && value == "1,000");
    fees = fees || (label == "Fees, all" && value == "6");
  }
  EXPECT_TRUE(total);
  EXPECT_TRUE(fees);

  EXPECT_FALSE(transfers(common(keys, 2, AccountIds::ID_FEE), {payments[0]}, 2)); // one is a transfer
  EXPECT_FALSE(transfers(common(keys, 2, AccountIds::ID_FEE), {payments[0], {AccountIds::ID_FEE, 1, 0}}, 2));
  EXPECT_FALSE(transfers(common(keys, 2, AccountIds::ID_FEE), {payments[0], {7, 0, 0}}, 2));
}

TEST(OperationTest, AnAccountReplacesItsKeysSignedByThoseInForce) {
  Keys inForce(2);
  Keys next(3);
  const uint64_t issuer = AccountIds::ID_FIRST_ISSUED + 9;
  UserAccount current;
  current.wallet.publicKeys = inForce.publicKeys();
  current.wallet.minSignatures = 2;
  current.wallet.mBalances[AccountIds::ID_GENESIS] = 500;
  current.wallet.mBalances[issuer] = -1000; // its own token, issued
  current.meta = "attachment";

  EXPECT_FALSE(replaceKeys(common(inForce, 2, issuer), current, next.publicKeys(), 4, 10)); // more than the keys
  EXPECT_FALSE(replaceKeys(common(inForce, 2, issuer), current, next.publicKeys(), 2, 501)); // cannot pay
  auto update = replaceKeys(common(inForce, 2, issuer), current, next.publicKeys(), 2, 10);
  ASSERT_TRUE(update) << update.error().message;
  EXPECT_EQ(update.value().kind, Kind::AccountUpdate);
  EXPECT_EQ(signerAccountOf(update.value().record), issuer);
  EXPECT_EQ(update.value().signers.publicKeys, inForce.publicKeys()); // the keys in force sign

  // It carries the account as it will be: new keys, the fee out of the native balance, the rest kept.
  auto tx = utl::binaryUnpack<Ledger::TxUserUpdate>(update.value().record.data).value();
  UserAccount carried;
  ASSERT_TRUE(carried.ltsFromString(tx.meta));
  EXPECT_EQ(carried.wallet.publicKeys, next.publicKeys());
  EXPECT_EQ(carried.wallet.mBalances.at(AccountIds::ID_GENESIS), 490);
  EXPECT_EQ(carried.wallet.mBalances.at(issuer), -1000);
  EXPECT_EQ(carried.meta, "attachment");

  auto s = summarize(update.value());
  ASSERT_TRUE(s) << s.error().message;
  EXPECT_EQ(s.value().title, "Update an account: its keys, its recovery");
  auto back = fromJson(toJson(update.value()));
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(back.value().kind, Kind::AccountUpdate);
  Operation otherSigners = update.value();
  otherSigners.signers.account = AccountIds::ID_RESERVE;
  EXPECT_FALSE(fromJson(toJson(otherSigners))); // the updated account signs, no other
}

TEST(OperationTest, AnOwnerSetsARecoveryItsSetAnnouncesAndTheOwnerCancels) {
  Keys owner(1);
  Keys guardians(3);
  Keys next(1);
  const uint64_t account = AccountIds::ID_FIRST_USER + 4;
  UserAccount current;
  current.wallet.publicKeys = owner.publicKeys();
  current.wallet.minSignatures = 1;
  current.wallet.mBalances[AccountIds::ID_GENESIS] = 500;

  // The owner sets it: the set, how many sign, the delay; keys and the rest kept.
  EXPECT_FALSE(setRecovery(common(owner, 1, account), current, guardians.publicKeys(), 4, 3, 10)); // 4 of 3
  EXPECT_FALSE(setRecovery(common(owner, 1, account), current, guardians.publicKeys(), 2, 0, 10)); // no delay
  auto set = setRecovery(common(owner, 1, account), current, guardians.publicKeys(), 2, 3, 10);
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_EQ(set.value().kind, Kind::AccountUpdate);
  auto carriedOf = [](const Operation &op) {
    UserAccount carried;
    EXPECT_TRUE(carried.ltsFromString(utl::binaryUnpack<Ledger::TxUserUpdate>(op.record.data).value().meta));
    return carried;
  };
  UserAccount carried = carriedOf(set.value());
  EXPECT_EQ(carried.wallet.publicKeys, owner.publicKeys());
  EXPECT_EQ(carried.wallet.recovery.publicKeys, guardians.publicKeys());
  EXPECT_EQ(carried.wallet.recovery.minSignatures, 2);
  EXPECT_EQ(carried.wallet.recovery.delayEpochs, 3u);
  EXPECT_EQ(carried.wallet.mBalances.at(AccountIds::ID_GENESIS), 490);
  EXPECT_FALSE(carriedOf(setRecovery(common(owner, 1, account), carried, {}, 0, 0, 10).value()).wallet.recovery.hasSet());

  // The set announces the next keys, signing for the account.
  EXPECT_FALSE(announceRecovery(common(guardians, 2, AccountIds::ID_FIRST_USER), account, next.publicKeys(), 1, 10));
  EXPECT_FALSE(announceRecovery(common(guardians, 2, account), account, next.publicKeys(), 2, 10)); // 2 of 1
  auto announce = announceRecovery(common(guardians, 2, account), account, next.publicKeys(), 1, 10);
  ASSERT_TRUE(announce) << announce.error().message;
  EXPECT_EQ(announce.value().kind, Kind::AccountRecovery);
  EXPECT_EQ(signerAccountOf(announce.value().record), account);
  auto s = summarize(announce.value());
  ASSERT_TRUE(s) << s.error().message;
  EXPECT_EQ(s.value().title, "Recover an account: new keys, after its delay");
  auto back = fromJson(toJson(announce.value()));
  ASSERT_TRUE(back) << back.error().message;
  EXPECT_EQ(back.value().kind, Kind::AccountRecovery);

  // Announced (as the chain holds it): the owner cancels; their updates never carry an announcement.
  EXPECT_FALSE(cancelRecovery(common(owner, 1, account), carried, 10)); // nothing announced
  carried.wallet.recovery.pending = true;
  carried.wallet.recovery.nextPublicKeys = next.publicKeys();
  carried.wallet.recovery.nextMinSignatures = 1;
  carried.wallet.recovery.applyEpoch = 9;
  carried.wallet.recovery.announcedAt = 70;
  auto cancel = cancelRecovery(common(owner, 1, account), carried, 10);
  ASSERT_TRUE(cancel) << cancel.error().message;
  UserAccount after = carriedOf(cancel.value());
  EXPECT_FALSE(after.wallet.recovery.pending);
  EXPECT_TRUE(after.wallet.recovery.nextPublicKeys.empty());
  EXPECT_EQ(after.wallet.recovery.applyEpoch, 0u);
  EXPECT_EQ(after.wallet.recovery.publicKeys, guardians.publicKeys()); // the set stays
  EXPECT_EQ(after.wallet.publicKeys, owner.publicKeys());
  auto replaced = replaceKeys(common(owner, 1, account), carried, next.publicKeys(), 1, 10);
  ASSERT_TRUE(replaced) << replaced.error().message;
  EXPECT_FALSE(carriedOf(replaced.value()).wallet.recovery.pending);
}

TEST(OperationTest, AmountsAreGroupedByThousands) {
  EXPECT_EQ(groupDigits(0), "0");
  EXPECT_EQ(groupDigits(999), "999");
  EXPECT_EQ(groupDigits(1000), "1,000");
  EXPECT_EQ(groupDigits(1073741824), "1,073,741,824");
}

} // namespace
