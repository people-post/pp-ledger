#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"
#include "AccountBuffer.h"
#include "Chain.h"
#include "BlockValidation.h"
#include "AccountPolicy.h"
#include "../consensus/EpochSeed.h"
#include "client/AccountAttachment.h"
#include "client/Client.h"
#include "TestTempPath.h"

#include <gtest/gtest.h>

#include <random>

#include <ctime>
#include <filesystem>
#include <functional>
#include <map>

using namespace pp;

namespace {

constexpr uint64_t BYTES_PER_KIB = 1024ULL;

uint64_t getFeeCoefficient(const std::vector<uint16_t> &coefficients,
                           size_t index) {
  return index < coefficients.size()
             ? static_cast<uint64_t>(coefficients[index])
             : 0ULL;
}

uint64_t
calculateMinimumFeeFromNonFreeMetaSize(const Chain::BlockChainConfig &config,
                                       uint64_t nonFreeBytes) {
  const uint64_t nonFreeSizeKiB =
      nonFreeBytes == 0 ? 0ULL
                        : (nonFreeBytes + BYTES_PER_KIB - 1) / BYTES_PER_KIB;

  const uint64_t a = getFeeCoefficient(config.minFeeCoefficients, 0);
  const uint64_t b = getFeeCoefficient(config.minFeeCoefficients, 1);
  const uint64_t c = getFeeCoefficient(config.minFeeCoefficients, 2);
  return a + b * nonFreeSizeKiB + c * nonFreeSizeKiB * nonFreeSizeKiB;
}

Chain::BlockChainConfig makeChainConfig(int64_t genesisTime) {
  Chain::BlockChainConfig cfg;
  cfg.genesisTime = genesisTime;
  cfg.slotDuration = 5;
  cfg.slotsPerEpoch = 10;
  cfg.maxCustomMetaSize = 1000;
  cfg.maxTransactionsPerBlock = 100;
  cfg.minFeeCoefficients = {1, 1, 0};
  cfg.freeCustomMetaSize = 512;
  cfg.checkpoint.minBlocks = 10;
  cfg.checkpoint.minAgeSeconds = 20;
  cfg.maxValidationTimespanSeconds = 86400;
  cfg.heartbeatSlots = 10; // == slotsPerEpoch default policy
  return cfg;
}

utl::MlDsaKeyPair makeKeyPair() {
  auto result = utl::mlDsaGenerate();
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

Client::UserAccount makeUserAccount(const std::string &publicKey,
                                    int64_t balance) {
  Client::UserAccount account;
  account.wallet.publicKeys = {publicKey};
  account.wallet.minSignatures = 1;
  account.wallet.keyType = Crypto::TK_ML_DSA_65;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.meta = AccountAttachment::emptySerialized();
  return account;
}

std::string signMessage(const utl::MlDsaKeyPair &keyPair,
                        const std::string &message) {
  auto result = utl::mlDsaSign(keyPair.privateKey, message);
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

template <typename TxT>
Ledger::Record makeRecord(uint16_t type, const TxT &tx,
                          const utl::MlDsaKeyPair &signer,
                          const std::string &networkId = {}) {
  Ledger::Record rec;
  rec.type = type;
  rec.data = utl::binaryPack(tx);
  rec.signatures = {signMessage(signer, rec.signingMessage(networkId))};
  return rec;
}

/** The generation-1 beacon signing key of every test chain (tests sign tips with it). */
const utl::MlDsaKeyPair &testBeaconKey() {
  static const utl::MlDsaKeyPair key = makeKeyPair();
  return key;
}

/** The fixture's recovery set: one key (1-of-1). */
const utl::MlDsaKeyPair &testRecoveryKey() {
  static const utl::MlDsaKeyPair key = makeKeyPair();
  return key;
}

/** A valid genesis authority for `networkId`: a 1-of-1 recovery set (testRecoveryKey()) and testBeaconKey(). */
Chain::ChainAuthority makeTestAuthority(const std::string &networkId) {
  Chain::ChainAuthority authority;
  authority.recovery.publicKeys = {testRecoveryKey().publicKey};
  authority.recovery.minSignatures = 1;
  authority.recovery.delayEpochs = 3;
  authority.beacon.networkId = networkId;
  authority.beacon.generation = 1;
  authority.beacon.publicKey = testBeaconKey().publicKey;
  authority.beacon.cutoverHeight = 0;
  return authority;
}

/** The fixture's genesis miner: the only stakeholder (system accounts never are). */
constexpr uint64_t kTestMinerId = AccountBuffer::ID_FIRST_ISSUED;
constexpr int64_t kTestMinerStake = 1000000;

/** The genesis block before sealing; `tweakMeta` edits the genesis meta (e.g. a bad authority). */
Ledger::ChainNode buildUnsealedGenesis(const Chain::BlockChainConfig &chainConfig,
                                       const utl::MlDsaKeyPair &genesisKey,
                                       const utl::MlDsaKeyPair &feeKey,
                                       const utl::MlDsaKeyPair &reserveKey,
                                       const utl::MlDsaKeyPair &recycleKey,
                                       const utl::MlDsaKeyPair *minerKey,
                                       const utl::MlDsaKeyPair *registrarKey,
                                       const std::function<void(Chain::GenesisAccountMeta &)> &tweakMeta) {
  // Genesis miner kTestMinerId signs with `minerKey` (default: the reserve key
  // pair, so tests signing as the slot leader keep using one key). The
  // registrar defaults to the recycle key pair.
  const utl::MlDsaKeyPair &minerSigner = minerKey ? *minerKey : reserveKey;
  const utl::MlDsaKeyPair &registrarSigner = registrarKey ? *registrarKey : recycleKey;
  Chain::GenesisAccountMeta gm;
  gm.config = chainConfig;
  gm.genesis.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  gm.genesis.wallet.publicKeys = {genesisKey.publicKey};
  gm.genesis.wallet.minSignatures = 1;
  gm.genesis.wallet.keyType = Crypto::TK_ML_DSA_65;
  gm.genesis.meta = AccountAttachment::emptySerialized();
  gm.authority = makeTestAuthority(chainConfig.networkId);
  if (tweakMeta) {
    tweakMeta(gm);
  }

  Ledger::ChainNode genesis;
  genesis.block.index = 0;
  genesis.block.timestamp = chainConfig.genesisTime;
  genesis.block.previousHash = utl::zeroHash();
  genesis.block.slot = 0;
  genesis.block.slotLeader = 0;
  genesis.block.epoch = 0;

  Ledger::TxGenesis checkpointTx;
  checkpointTx.fee = 0;
  checkpointTx.meta = gm.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_GENESIS, checkpointTx, genesisKey,
                 chainConfig.networkId));

  auto minFee = [&](const Client::UserAccount &account) {
    const uint64_t nonFree = account.meta.size() > chainConfig.freeCustomMetaSize
                                 ? static_cast<uint64_t>(account.meta.size()) - chainConfig.freeCustomMetaSize
                                 : 0ULL;
    return calculateMinimumFeeFromNonFreeMetaSize(chainConfig, nonFree);
  };
  auto newAccountTx = [&](uint64_t toId, const Client::UserAccount &account) {
    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountBuffer::ID_GENESIS;
    tx.toWalletId = toId;
    tx.amount = static_cast<uint64_t>(account.wallet.mBalances.at(AccountBuffer::ID_GENESIS));
    tx.fee = minFee(account);
    tx.meta = account.ltsToString();
    return tx;
  };

  // Same layout as Beacon::createGenesisBlock: fee, reserve, registrar,
  // recycle, then the genesis miner.
  const Ledger::TxNewUser feeTx = newAccountTx(AccountBuffer::ID_FEE, makeUserAccount(feeKey.publicKey, 0));
  const Ledger::TxNewUser registrarTx =
      newAccountTx(AccountBuffer::ID_REGISTRAR, makeUserAccount(registrarSigner.publicKey, 0));
  const Ledger::TxNewUser recycleTx =
      newAccountTx(AccountBuffer::ID_RECYCLE, makeUserAccount(recycleKey.publicKey, 0));
  Client::UserAccount minerAccount = makeUserAccount(minerSigner.publicKey, kTestMinerStake);
  minerAccount.wallet.miner.registered = true;  // genesis miners are registered from block 0
  const Ledger::TxNewUser minerTx = newAccountTx(kTestMinerId, minerAccount);

  const uint64_t reserveFee = minFee(makeUserAccount(reserveKey.publicKey, 0));
  const int64_t reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY) -
                                static_cast<int64_t>(feeTx.fee + registrarTx.fee + recycleTx.fee + minerTx.fee +
                                                     reserveFee) -
                                kTestMinerStake;
  const Ledger::TxNewUser reserveTx =
      newAccountTx(AccountBuffer::ID_RESERVE, makeUserAccount(reserveKey.publicKey, reserveAmount));

  for (const auto *tx : {&feeTx, &reserveTx, &registrarTx, &recycleTx, &minerTx}) {
    genesis.block.records.push_back(makeRecord(Ledger::T_NEW_USER, *tx, genesisKey, chainConfig.networkId));
  }
  return genesis;
}

Ledger::ChainNode makeGenesisBlock(Chain &validator,
                                   const Chain::BlockChainConfig &chainConfig,
                                   const utl::MlDsaKeyPair &genesisKey,
                                   const utl::MlDsaKeyPair &feeKey,
                                   const utl::MlDsaKeyPair &reserveKey,
                                   const utl::MlDsaKeyPair &recycleKey,
                                   const utl::MlDsaKeyPair *minerKey = nullptr,
                                   const utl::MlDsaKeyPair *registrarKey = nullptr) {
  Ledger::ChainNode genesis =
      buildUnsealedGenesis(chainConfig, genesisKey, feeKey, reserveKey, recycleKey, minerKey, registrarKey, {});
  auto sealResult = validator.sealBlock(genesis);
  EXPECT_TRUE(sealResult.isOk());
  return genesis;
}

Ledger::ChainNode makeNextBlockAtSlot(
    Chain &validator, const Ledger::ChainNode &previous, uint64_t slot,
    const std::vector<Ledger::Record> &records) {
  const uint64_t epoch = validator.getEpochFromSlot(slot);
  // The block's epoch committee, as admission derives it (a new epoch snapshots the tip bank).
  validator.refreshStakeholders(slot);
  auto seedRoe = validator.ensureEpochSeed(epoch);
  EXPECT_TRUE(seedRoe.isOk()) << (seedRoe.isOk() ? "" : seedRoe.error().message);
  auto leaderResult = validator.getSlotLeader(slot);
  EXPECT_TRUE(leaderResult.isOk())
      << (leaderResult.isOk() ? "" : leaderResult.error().message);
  auto block = validator.linkNextBlock(
      previous, slot, leaderResult.isOk() ? leaderResult.value() : 0,
      validator.getSlotEndTime(slot), records);
  auto sealResult = validator.sealBlock(block);
  EXPECT_TRUE(sealResult.isOk())
      << (sealResult.isOk() ? "" : sealResult.error().message);
  return block;
}

Ledger::ChainNode makeNextBlock(
    Chain &validator, const Ledger::ChainNode &previous,
    const std::vector<Ledger::Record> &records) {
  return makeNextBlockAtSlot(validator, previous, previous.block.slot + 1,
                             records);
}

} // namespace

TEST(ChainTest, GenesisAccountMeta_RoundTrip) {
  Chain::GenesisAccountMeta gm;
  gm.config = makeChainConfig(12345);
  gm.genesis = makeUserAccount("pk", 0);

  std::string serialized = gm.ltsToString();
  Chain::GenesisAccountMeta parsed;
  EXPECT_TRUE(parsed.ltsFromString(serialized));
  EXPECT_EQ(parsed.config.genesisTime, gm.config.genesisTime);
  EXPECT_EQ(parsed.config.slotDuration, gm.config.slotDuration);
  EXPECT_EQ(parsed.config.slotsPerEpoch, gm.config.slotsPerEpoch);
  EXPECT_EQ(parsed.config.maxCustomMetaSize, gm.config.maxCustomMetaSize);
  EXPECT_EQ(parsed.config.maxTransactionsPerBlock,
            gm.config.maxTransactionsPerBlock);
  EXPECT_EQ(parsed.config.minFeeCoefficients, gm.config.minFeeCoefficients);
  EXPECT_EQ(parsed.config.freeCustomMetaSize, gm.config.freeCustomMetaSize);
  EXPECT_EQ(parsed.config.checkpoint.minBlocks, gm.config.checkpoint.minBlocks);
  EXPECT_EQ(parsed.config.checkpoint.minAgeSeconds,
            gm.config.checkpoint.minAgeSeconds);
  EXPECT_EQ(parsed.config.heartbeatSlots, gm.config.heartbeatSlots);
  EXPECT_EQ(parsed.genesis.wallet.publicKeys, gm.genesis.wallet.publicKeys);
  EXPECT_EQ(parsed.genesis.wallet.minSignatures,
            gm.genesis.wallet.minSignatures);
  EXPECT_EQ(parsed.genesis.wallet.mBalances, gm.genesis.wallet.mBalances);
}

TEST(ChainTest, CalculateHash_DeterministicAndSensitive) {
  Chain validator;

  Ledger::Block block;
  block.index = 1;
  block.timestamp = 12345;
  block.previousHash = utl::zeroHash();
  block.slot = 2;
  block.slotLeader = 3;
  block.txRoot = utl::sha256Raw("txroot");
  block.stateRoot = utl::sha256Raw("stateroot");

  std::string hash1 = validator.calculateHash(block);
  std::string hash2 = validator.calculateHash(block);
  EXPECT_EQ(hash1, hash2);
  EXPECT_EQ(hash1.size(), utl::SHA256_DIGEST_SIZE);

  block.slotLeader = 4;
  std::string hash3 = validator.calculateHash(block);
  EXPECT_NE(hash1, hash3);

  // Body changes alone must not affect header hash when roots are unchanged.
  block.slotLeader = 3;
  block.records.push_back({});
  EXPECT_EQ(validator.calculateHash(block), hash1);

  block.txRoot = utl::sha256Raw("txroot-changed");
  EXPECT_NE(validator.calculateHash(block), hash1);
}

// beacon-authority ba1: one rule per refusal (B004 / B006).
TEST(ChainTest, GenesisAuthority_RefusesEveryBadShape) {
  const auto admin = makeKeyPair();
  Client::Wallet adminWallet;
  adminWallet.publicKeys = {admin.publicKey};
  const Chain::ChainAuthority valid = makeTestAuthority("test-net");
  ASSERT_TRUE(chain_tx::validateGenesisAuthority(valid, adminWallet, "test-net").isOk());

  const std::vector<std::pair<std::string, std::function<void(Chain::ChainAuthority &)>>> refused = {
      {"at least one public key", [](auto &a) { a.recovery.publicKeys.clear(); }},
      {"malformed public key", [](auto &a) { a.recovery.publicKeys = {"short"}; }},
      {"same key twice", [](auto &a) { a.recovery.publicKeys.push_back(a.recovery.publicKeys[0]); }},
      {"must not be an admin", [&](auto &a) { a.recovery.publicKeys = {admin.publicKey}; }},
      {"between 1 and its key count", [](auto &a) { a.recovery.minSignatures = 0; }},
      {"between 1 and its key count", [](auto &a) { a.recovery.minSignatures = 2; }},
      {"delayEpochs must be at least 1", [](auto &a) { a.recovery.delayEpochs = 0; }},
      {"must be generation 1", [](auto &a) { a.beacon.generation = 2; }},
      {"cutoverHeight 0", [](auto &a) { a.beacon.cutoverHeight = 5; }},
      {"networkId does not match", [](auto &a) { a.beacon.networkId = "other-net"; }},
      {"Beacon certificate has a malformed", [](auto &a) { a.beacon.publicKey = "short"; }},
      {"not be an admin or recovery key", [&](auto &a) { a.beacon.publicKey = admin.publicKey; }},
      {"not be an admin or recovery key", [](auto &a) { a.beacon.publicKey = a.recovery.publicKeys[0]; }},
      {"cannot carry a pending recovery", [](auto &a) { a.hasPendingRecovery = true; }},
  };
  for (const auto &[why, change] : refused) {
    Chain::ChainAuthority authority = valid;
    change(authority);
    auto result = chain_tx::validateGenesisAuthority(authority, adminWallet, "test-net");
    ASSERT_FALSE(result.isOk()) << why;
    EXPECT_NE(result.error().message.find(why), std::string::npos) << result.error().message;
  }
}

// The genesis record must carry a valid authority; a valid one is in force afterwards.
TEST(ChainTest, Genesis_SetsTheAuthorityAndRefusesAnInvalidOne) {
  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  for (const auto &tweak : std::vector<std::function<void(Chain::GenesisAccountMeta &)>>{
           [](auto &gm) { gm.authority = {}; },
           [&](auto &gm) { gm.authority.recovery.publicKeys = {genesisKey.publicKey}; },
       }) {
    Chain refuser;
    Ledger::ChainNode bad = buildUnsealedGenesis(chainConfig, genesisKey, feeKey, reserveKey, recycleKey,
                                                 nullptr, nullptr, tweak);
    EXPECT_FALSE(refuser.sealBlock(bad).isOk());
    EXPECT_FALSE(refuser.getAuthority().has_value());
  }

  Chain validator;
  makeGenesisBlock(validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto authority = validator.getAuthority();
  ASSERT_TRUE(authority.has_value());
  EXPECT_EQ(authority->beacon.generation, 1u);
  EXPECT_EQ(authority->beacon.networkId, chainConfig.networkId);
  EXPECT_EQ(authority->recovery.publicKeys.size(), 1u);
}

TEST(ChainTest, AddBlock_FailsOnGenesisHashMismatch) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  // sealBlock already applied tip state; corrupting hash must not commit.
  genesis.hash = "bad-hash";

  auto result = validator.addBlock(genesis);
  EXPECT_TRUE(result.isError());
  EXPECT_NE(result.error().message.find("uncommitted sealed block"),
            std::string::npos);
}

TEST(ChainTest, AddBlock_AddsValidGenesisBlock) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);

  auto result = validator.addBlock(genesis);
  EXPECT_TRUE(result.isOk());
  EXPECT_EQ(validator.getNextBlockId(), 1u);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionsByWalletId_ReturnsEmptyWhenChainHasNoBlocks) {
  Chain validator;

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-findtx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());
  ASSERT_EQ(validator.getNextBlockId(), 0u);

  uint64_t blockId = 0;
  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_GENESIS, blockId);
  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result.value().empty());
  EXPECT_EQ(blockId, 0u);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest,
     FindTransactionsByWalletId_WhenStartBlockIdZero_ScansFromLatest) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-findtx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  uint64_t blockId = 0;
  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_GENESIS, blockId);
  ASSERT_TRUE(result.isOk());
  EXPECT_FALSE(result.value().empty());
  EXPECT_EQ(blockId, 0u);
  for (const auto &st : result.value()) {
    bool matches = false;
    switch (st.type) {
    case Ledger::T_DEFAULT: {
      auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.fromWalletId == AccountBuffer::ID_GENESIS ||
                 tx.toWalletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_GENESIS: {
      auto txRoe = utl::binaryUnpack<Ledger::TxGenesis>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      matches = true;
      break;
    }
    case Ledger::T_NEW_USER: {
      auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.fromWalletId == AccountBuffer::ID_GENESIS ||
                 tx.toWalletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_CONFIG: {
      auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      matches = true;
      break;
    }
    case Ledger::T_USER_UPDATE: {
      auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_RENEWAL: {
      auto txRoe = utl::binaryUnpack<Ledger::TxRenewal>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_END_USER: {
      auto txRoe = utl::binaryUnpack<Ledger::TxEndUser>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    default:
      break;
    }
    EXPECT_TRUE(matches);
  }

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionsByWalletId_ReturnsTransactionsInvolvingWallet) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-findtx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  uint64_t blockId = validator.getNextBlockId();
  ASSERT_GT(blockId, 0u);

  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_GENESIS, blockId);
  ASSERT_TRUE(result.isOk());
  const auto &txes = result.value();
  EXPECT_FALSE(txes.empty());
  for (const auto &st : txes) {
    bool matches = false;
    switch (st.type) {
    case Ledger::T_DEFAULT: {
      auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.fromWalletId == AccountBuffer::ID_GENESIS ||
                 tx.toWalletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_GENESIS: {
      auto txRoe = utl::binaryUnpack<Ledger::TxGenesis>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      matches = true;
      break;
    }
    case Ledger::T_NEW_USER: {
      auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.fromWalletId == AccountBuffer::ID_GENESIS ||
                 tx.toWalletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_CONFIG: {
      auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      matches = true;
      break;
    }
    case Ledger::T_USER_UPDATE: {
      auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_RENEWAL: {
      auto txRoe = utl::binaryUnpack<Ledger::TxRenewal>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    case Ledger::T_END_USER: {
      auto txRoe = utl::binaryUnpack<Ledger::TxEndUser>(st.data);
      ASSERT_TRUE(txRoe.isOk());
      const auto &tx = txRoe.value();
      matches = (tx.walletId == AccountBuffer::ID_GENESIS);
      break;
    }
    default:
      break;
    }
    EXPECT_TRUE(matches);
  }
  EXPECT_EQ(blockId, 0u);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionsByWalletId_ReturnsEmptyForUnknownWallet) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-findtx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  constexpr uint64_t unknownWalletId = 9999;
  uint64_t blockId = validator.getNextBlockId();
  auto result = validator.findTransactionsByWalletId(unknownWalletId, blockId);
  ASSERT_TRUE(result.isOk());
  EXPECT_TRUE(result.value().empty());
  EXPECT_EQ(blockId, 0u);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionsByWalletId_ClampsBlockIdToNextBlockId) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-findtx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  uint64_t nextBlockId = validator.getNextBlockId();
  uint64_t blockId = nextBlockId + 1000u;
  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_FEE, blockId);
  ASSERT_TRUE(result.isOk());
  EXPECT_GT(result.value().size(), 0u);
  EXPECT_EQ(blockId, 0u);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionByIndex_ReturnsErrorWhenNoBlocks) {
  Chain validator;

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-gettx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());
  ASSERT_EQ(validator.getNextBlockId(), 0u);

  auto result = validator.findTransactionByIndex(0);
  ASSERT_TRUE(result.isError());
  EXPECT_NE(result.error().message.find("No blocks"), std::string::npos);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FindTransactionByIndex_ReturnsErrorWhenIndexOutOfRange) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-gettx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  // Indices 0..n-1 are genesis transactions; index n is out of range.
  auto result = validator.findTransactionByIndex(genesis.block.records.size());
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.error().code, Chain::E_INVALID_ARGUMENT);
  EXPECT_NE(result.error().message.find("out of range"), std::string::npos);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest,
     FindTransactionByIndex_ReturnsCorrectTransactionInGenesisBlock) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-gettx");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addResult = validator.addBlock(genesis);
  ASSERT_TRUE(addResult.isOk());

  // Genesis block has 4 transactions at indices 0..3.
  for (size_t i = 0; i < genesis.block.records.size(); ++i) {
    auto result = validator.findTransactionByIndex(static_cast<uint64_t>(i));
    ASSERT_TRUE(result.isOk()) << "findTransactionByIndex(" << i << ") failed";
    EXPECT_EQ(result.value().type, genesis.block.records[i].type);

    auto unpackWallets = [&](uint16_t type, const std::string &data)
        -> std::pair<uint64_t, uint64_t> {
      switch (type) {
      case Ledger::T_DEFAULT: {
        auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {txRoe.value().fromWalletId, txRoe.value().toWalletId};
      }
      case Ledger::T_GENESIS: {
        auto txRoe = utl::binaryUnpack<Ledger::TxGenesis>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS};
      }
      case Ledger::T_NEW_USER: {
        auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {txRoe.value().fromWalletId, txRoe.value().toWalletId};
      }
      case Ledger::T_CONFIG: {
        auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {AccountBuffer::ID_GENESIS, AccountBuffer::ID_GENESIS};
      }
      case Ledger::T_USER_UPDATE: {
        auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {txRoe.value().walletId, txRoe.value().walletId};
      }
      case Ledger::T_RENEWAL: {
        auto txRoe = utl::binaryUnpack<Ledger::TxRenewal>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {txRoe.value().walletId, txRoe.value().walletId};
      }
      case Ledger::T_END_USER: {
        auto txRoe = utl::binaryUnpack<Ledger::TxEndUser>(data);
        EXPECT_TRUE(txRoe.isOk());
        if (!txRoe.isOk()) return {0, 0};
        return {txRoe.value().walletId, txRoe.value().walletId};
      }
      default:
        return {0, 0};
      }
    };

    const auto [gotFrom, gotTo] = unpackWallets(result.value().type, result.value().data);
    const auto [expFrom, expTo] = unpackWallets(genesis.block.records[i].type, genesis.block.records[i].data);
    EXPECT_EQ(gotFrom, expFrom);
    EXPECT_EQ(gotTo, expTo);
  }

  std::filesystem::remove_all(tempDir, ec);
}

// Regression: live tip admission must reject re-adding an already-committed
// block index, even when the resubmitted block is byte-for-byte the original
// (structurally valid) one. Without this, an old block could be replayed on
// top of the advanced tip and double-apply its transactions.
// Regression: a zero idempotentId used to skip cross-block replay
// protection entirely (chain_tx::checkIdempotency short-circuits on 0).
// Fund-moving tx types must now reject id 0 outright in Full mode.
TEST(ChainTest, AddBlock_RejectsDefaultTransferWithZeroIdempotentId) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-zero-idempotent-id");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(validator.addBlock(genesis).isOk());

  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 100;
  tx.fee = 1;
  tx.idempotentId = 0; // must be rejected, not silently skip replay checks
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  tx.meta.clear();
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  validator.refreshStakeholders();
  const uint64_t slot = genesis.block.slot + 1;
  const uint64_t epoch = validator.getEpochFromSlot(slot);
  ASSERT_TRUE(validator.ensureEpochSeed(epoch).isOk());
  auto leaderResult = validator.getSlotLeader(slot);
  ASSERT_TRUE(leaderResult.isOk());
  auto block1 = validator.linkNextBlock(
      genesis, slot, leaderResult.value(), validator.getSlotEndTime(slot),
      {rec});
  auto sealResult = validator.sealBlock(block1);
  ASSERT_FALSE(sealResult.isOk());
  EXPECT_NE(sealResult.error().message.find("idempotentId"), std::string::npos);

  std::filesystem::remove_all(tempDir, ec);
}

// Regression: a block's slot must strictly increase over its predecessor.
// validateBlockTiming only checked that the timestamp matched the block's
// own claimed slot, so a leader (or forged block) could reuse or rewind the
// slot number as long as it self-reported a matching timestamp.
TEST(ChainTest, Seal_RejectsNonIncreasingSlot) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir = pp::test::uniqueTempPath("pp-ledger-chain-test-nonincreasing-slot");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  ASSERT_TRUE(validator.initLedger(ledgerConfig).isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(validator.addBlock(genesis).isOk());

  // Non-empty so the heartbeat-lag policy doesn't also reject it.
  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 10;
  tx.fee = 1;
  tx.idempotentId = 1;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  tx.meta.clear();
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  validator.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(validator, genesis, {rec});
  ASSERT_TRUE(validator.addBlock(block1).isOk());

  // Attempt a block2 that reuses block1's own slot instead of advancing.
  // Built manually (not via makeNextBlockAtSlot, which asserts seal
  // success). The slot check runs before seal applies anything.
  Ledger::TxDefault tx2;
  tx2.tokenId = AccountBuffer::ID_GENESIS;
  tx2.fromWalletId = AccountBuffer::ID_RESERVE;
  tx2.toWalletId = AccountBuffer::ID_FEE;
  tx2.amount = 10;
  tx2.fee = 1;
  tx2.idempotentId = 2;
  tx2.validationTsMin = chainConfig.genesisTime;
  tx2.validationTsMax = chainConfig.genesisTime + 3600;
  tx2.meta.clear();
  Ledger::Record rec2 = makeRecord(Ledger::T_DEFAULT, tx2, reserveKey);

  validator.refreshStakeholders();
  const uint64_t badSlot = block1.block.slot;
  const uint64_t epoch = validator.getEpochFromSlot(badSlot);
  ASSERT_TRUE(validator.ensureEpochSeed(epoch).isOk());
  auto leaderResult = validator.getSlotLeader(badSlot);
  ASSERT_TRUE(leaderResult.isOk());
  auto badBlock = validator.linkNextBlock(
      block1, badSlot, leaderResult.value(),
      validator.getSlotEndTime(badSlot), {rec2});
  auto reserveBefore = validator.getAccount(AccountBuffer::ID_RESERVE);
  ASSERT_TRUE(reserveBefore.isOk());
  auto sealResult = validator.sealBlock(badBlock);
  ASSERT_FALSE(sealResult.isOk());
  EXPECT_NE(sealResult.error().message.find("Invalid block slot"),
            std::string::npos);

  // Failed seal left no pending seal and no applied effects.
  auto reserveAfter = validator.getAccount(AccountBuffer::ID_RESERVE);
  ASSERT_TRUE(reserveAfter.isOk());
  EXPECT_EQ(reserveAfter.value().wallet.mBalances,
            reserveBefore.value().wallet.mBalances);
  Ledger::ChainNode next =
      makeNextBlockAtSlot(validator, block1, badSlot + 1, {rec2});
  auto addResult = validator.addBlock(next);
  ASSERT_TRUE(addResult.isOk()) << addResult.error().message;

  std::filesystem::remove_all(tempDir, ec);
}

// Regression: a block's slot must not be far beyond the local clock's
// current slot -- otherwise a block could claim an arbitrarily-future slot
// (with a self-consistent timestamp) and be accepted.
TEST(ChainTest, Seal_RejectsFarFutureSlot) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir = pp::test::uniqueTempPath("pp-ledger-chain-test-far-future-slot");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  ASSERT_TRUE(validator.initLedger(ledgerConfig).isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(validator.addBlock(genesis).isOk());

  // Pin the clock to slot 0's start so the check is deterministic, and stay
  // within epoch 0 (slotsPerEpoch=10) so genesis's epoch seed still covers
  // it -- only the future-slot bound is under test here.
  validator.setClockOverride(validator.getSlotStartTime(0));
  validator.refreshStakeholders();
  const uint64_t farFutureSlot = 5; // > currentSlot(0) + tolerance(2)
  // Built manually (not via makeNextBlockAtSlot, which asserts seal
  // success) since this seal is expected to fail.
  const uint64_t epoch = validator.getEpochFromSlot(farFutureSlot);
  ASSERT_TRUE(validator.ensureEpochSeed(epoch).isOk());
  auto leaderResult = validator.getSlotLeader(farFutureSlot);
  ASSERT_TRUE(leaderResult.isOk());
  auto badBlock = validator.linkNextBlock(
      genesis, farFutureSlot, leaderResult.value(),
      validator.getSlotEndTime(farFutureSlot), {});
  auto sealResult = validator.sealBlock(badBlock);
  ASSERT_FALSE(sealResult.isOk());
  EXPECT_NE(sealResult.error().message.find("too far in the future"),
            std::string::npos);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, FutureSlotTolerance_IsMaxOfTwoSlotsAndFifteenSeconds) {
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(0), 2u);
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(1), 15u);
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(5), 3u);
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(4), 4u);
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(10), 2u);
  EXPECT_EQ(chain_block::futureSlotToleranceSlots(60), 2u);
}

// Slot monotonicity is an arrival rule only: the structural layer used by
// ledger replay accepts a same-slot successor, checkBlockArrival rejects it.
TEST(ChainTest, SlotMonotonicity_ArrivalOnlyNotStructural) {
  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-arrival");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  Ledger ledger;
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  ASSERT_TRUE(ledger.init(ledgerConfig).isOk());

  auto makeBlock = [](uint64_t index, uint64_t slot,
                      const std::string &previousHash) {
    Ledger::ChainNode node;
    node.block.index = index;
    node.block.slot = slot;
    node.block.previousHash = previousHash;
    node.block.txRoot = chain_block::calculateTxRoot(node.block.records);
    node.hash = chain_block::calculateBlockHash(node.block);
    return node;
  };
  Ledger::ChainNode b0 = makeBlock(0, 0, utl::zeroHash());
  ASSERT_TRUE(ledger.addBlock(b0).isOk());
  Ledger::ChainNode b1 = makeBlock(1, 4, b0.hash);
  ASSERT_TRUE(ledger.addBlock(b1).isOk());
  Ledger::ChainNode sameSlot = makeBlock(2, 4, b1.hash);

  EXPECT_TRUE(chain_block::checkBlockStructural(sameSlot, ledger).isOk());

  consensus::SlotCommittee consensus;
  consensus::SlotCommittee::Config config;
  config.genesisTime = 0;
  config.slotDuration = 5;
  config.slotsPerEpoch = 10;
  consensus.init(config);
  consensus.setClockOverride(consensus.getSlotStartTime(4));
  auto arrival = chain_block::checkBlockArrival(sameSlot, ledger, consensus);
  ASSERT_FALSE(arrival.isOk());
  EXPECT_NE(arrival.error().message.find("Invalid block slot"),
            std::string::npos);
  EXPECT_TRUE(chain_block::checkBlockArrival(makeBlock(2, 5, b1.hash), ledger,
                                             consensus)
                  .isOk());

  std::filesystem::remove_all(tempDir, ec);
}

// Reload must not apply the future-slot bound: a node whose clock is behind
// the stored history (skew, restored backup) still loads its own ledger.
TEST(ChainTest, LoadFromLedger_IgnoresFutureSlotBound) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir = pp::test::uniqueTempPath("pp-ledger-chain-test-reload-future");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  ASSERT_TRUE(validator.initLedger(ledgerConfig).isOk());
  validator.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock()); // replayed by a second chain below

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(validator.addBlock(genesis).isOk());

  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 10;
  tx.fee = 1;
  tx.idempotentId = 1;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  const uint64_t slot = 5;
  const int64_t slot0Start = validator.getSlotStartTime(0);
  validator.setClockOverride(validator.getSlotStartTime(slot));
  validator.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlockAtSlot(validator, genesis, slot, {rec});
  ASSERT_TRUE(validator.addBlock(block1).isOk());

  // Replay with the clock at slot 0: slot 5 > 0 + tolerance(3) for arrivals.
  Chain replay;
  replay.initConsensus(consensusConfig);
  replay.setClockOverride(slot0Start);
  ASSERT_TRUE(replay.mountLedger(tempDir.string()).isOk());
  auto loaded = replay.loadFromLedger(0);
  ASSERT_TRUE(loaded.isOk()) << loaded.error().message;
  EXPECT_EQ(loaded.value(), 2u);

  std::filesystem::remove_all(tempDir, ec);
}

// Regression: chain-generated renewals map to idempotentId == 0 and
// must stay exempt from the non-zero rule, or the first renewal-due block
// can never be produced and the chain stalls.
TEST(ChainTest, Renewal_ZeroIdempotentIdAccepted) {
  Chain producer;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(0);
  chainConfig.checkpoint.minBlocks = 2;
  chainConfig.checkpoint.minAgeSeconds = 0;

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  producer.initConsensus(consensusConfig);

  const auto tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-renewal");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.startingBlockId = 0;
  ledgerConfig.workDir = tempDir.string();
  ASSERT_TRUE(producer.initLedger(ledgerConfig).isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      producer, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(producer.addBlock(genesis).isOk());

  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 10;
  tx.fee = 1;
  tx.idempotentId = 1;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  producer.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(producer, genesis, {rec});
  ASSERT_TRUE(producer.addBlock(block1).isOk());

  // minBlocks = 2: the genesis-era accounts are now due for renewal.
  const uint64_t slot2 = block1.block.slot + 1;
  auto renewals = producer.collectRenewals(slot2);
  ASSERT_TRUE(renewals.isOk()) << renewals.error().message;
  ASSERT_FALSE(renewals.value().empty());

  // Renewals are signed by the slot leader (as Miner::initSlotCache does).
  ASSERT_TRUE(producer.ensureEpochSeed(producer.getEpochFromSlot(slot2)).isOk());
  auto leader = producer.getSlotLeader(slot2);
  ASSERT_TRUE(leader.isOk());
  const std::map<uint64_t, const utl::MlDsaKeyPair *> keys = {
      {AccountBuffer::ID_GENESIS, &genesisKey},
      {AccountBuffer::ID_FEE, &feeKey},
      {AccountBuffer::ID_RESERVE, &reserveKey},
      {AccountBuffer::ID_RECYCLE, &recycleKey},
      {kTestMinerId, &reserveKey}};
  ASSERT_EQ(keys.count(leader.value()), 1u);

  // Same admission path the slot leader uses for renewals; block apply goes
  // through the same UserAccountUpsertBase flag. Genesis (own handler path,
  // fixture has a single key) and fee (self-paid fee) are not covered here.
  size_t userRenewals = 0;
  for (auto r : renewals.value()) {
    if (r.type != Ledger::T_RENEWAL) {
      continue;
    }
    auto renewal = utl::binaryUnpack<Ledger::TxRenewal>(r.data);
    ASSERT_TRUE(renewal.isOk());
    if (renewal.value().walletId == AccountBuffer::ID_GENESIS ||
        renewal.value().walletId == AccountBuffer::ID_FEE) {
      continue;
    }
    auto sig = utl::mlDsaSign(keys.at(leader.value())->privateKey,
                              r.signingMessage(producer.getNetworkId()));
    ASSERT_TRUE(sig.isOk());
    r.signatures.push_back(sig.value());
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(scratch, r, leader.value());
    EXPECT_TRUE(added.isOk()) << added.error().message;
    ++userRenewals;
  }
  EXPECT_GT(userRenewals, 0u);

  std::filesystem::remove_all(tempDir, ec);
}

// Regression: the recycle account (balance 0) used to get T_END_USER at its
// first renewal and be deleted, after which every write-off failed. System
// accounts (AccountIds::isSystemAccount) are never terminated and renew
// fee-free; validators enforce both.
TEST(ChainTest, SystemAccounts_RenewFeeFreeAndAreNeverTerminated) {
  Chain producer;
  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(0);
  chainConfig.checkpoint.minBlocks = 2;
  chainConfig.checkpoint.minAgeSeconds = 0;
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  producer.initConsensus(consensusConfig);

  const auto tempDir = pp::test::uniqueTempPath("pp-ledger-chain-test-system-accounts");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.startingBlockId = 0;
  ledgerConfig.workDir = tempDir.string();
  ASSERT_TRUE(producer.initLedger(ledgerConfig).isOk());

  Ledger::ChainNode genesis =
      makeGenesisBlock(producer, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(producer.addBlock(genesis).isOk());
  ASSERT_EQ(producer.getAccount(AccountBuffer::ID_RECYCLE).value().wallet.mBalances.count(AccountBuffer::ID_GENESIS) == 0 ||
                producer.getAccount(AccountBuffer::ID_RECYCLE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS) == 0,
            true); // recycle starts empty: it cannot pay a renewal fee

  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 10;
  tx.fee = 1;
  tx.idempotentId = 1;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  producer.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(producer, genesis, {makeRecord(Ledger::T_DEFAULT, tx, reserveKey)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());

  const uint64_t slot2 = block1.block.slot + 1;
  auto renewals = producer.collectRenewals(slot2);
  ASSERT_TRUE(renewals.isOk()) << renewals.error().message;
  ASSERT_TRUE(producer.ensureEpochSeed(producer.getEpochFromSlot(slot2)).isOk());
  auto leader = producer.getSlotLeader(slot2);
  ASSERT_TRUE(leader.isOk());
  const std::map<uint64_t, const utl::MlDsaKeyPair *> keys = {{AccountBuffer::ID_GENESIS, &genesisKey},
                                                             {AccountBuffer::ID_FEE, &feeKey},
                                                             {AccountBuffer::ID_RESERVE, &reserveKey},
                                                             {AccountBuffer::ID_RECYCLE, &recycleKey},
      {kTestMinerId, &reserveKey}};
  const auto &leaderKey = *keys.at(leader.value());
  auto signByLeader = [&](Ledger::Record r) {
    r.signatures = {utl::mlDsaSign(leaderKey.privateKey, r.signingMessage(producer.getNetworkId())).value()};
    return r;
  };

  std::optional<Ledger::Record> recycleRenewal;
  for (const auto &r : renewals.value()) {
    ASSERT_NE(r.type, Ledger::T_END_USER) << "a system account must never be terminated";
    auto renewal = utl::binaryUnpack<Ledger::TxRenewal>(r.data);
    ASSERT_TRUE(renewal.isOk());
    if (renewal.value().walletId == AccountBuffer::ID_RECYCLE) {
      EXPECT_EQ(renewal.value().fee, 0u);
      recycleRenewal = signByLeader(r);
    }
  }
  ASSERT_TRUE(recycleRenewal.has_value());
  {
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(scratch, *recycleRenewal, leader.value());
    EXPECT_TRUE(added.isOk()) << added.error().message;
  }

  // Validators refuse: a fee on a system renewal, terminating a system
  // account, and creating a system-range id after genesis.
  {
    auto renewal = utl::binaryUnpack<Ledger::TxRenewal>(recycleRenewal->data).value();
    renewal.fee = 1;
    Ledger::Record charged = *recycleRenewal;
    charged.data = utl::binaryPack(renewal);
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(scratch, signByLeader(charged), leader.value());
    EXPECT_FALSE(added.isOk());
  }
  {
    Ledger::TxEndUser end;
    end.walletId = AccountBuffer::ID_RECYCLE;
    Ledger::Record r;
    r.type = Ledger::T_END_USER;
    r.data = utl::binaryPack(end);
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(scratch, signByLeader(r), leader.value());
    ASSERT_FALSE(added.isOk());
    EXPECT_NE(added.error().message.find("cannot be terminated"), std::string::npos) << added.error().message;
  }
  {
    Ledger::TxNewUser nu;
    nu.fromWalletId = AccountBuffer::ID_GENESIS;
    nu.toWalletId = 10; // system range
    nu.idempotentId = 7;
    const int64_t now = static_cast<int64_t>(std::time(nullptr)); // buffer admission checks wall time
    nu.validationTsMin = now - 60;
    nu.validationTsMax = now + 60;
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_NEW_USER, nu, genesisKey, producer.getNetworkId()), leader.value());
    ASSERT_FALSE(added.isOk());
    EXPECT_NE(added.error().message.find("genesis block"), std::string::npos) << added.error().message;
  }

  // Fee-free system accounts renew before anything else in the block: other
  // records credit them (genesis renewal fee, write-offs), and each renewal
  // carries the committed balance the validator checks when it applies.
  {
    bool seenOther = false;
    for (const auto &r : renewals.value()) {
      const uint64_t id = utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().walletId;
      const bool feeFreeSystem = AccountIds::isSystemAccount(id);
      EXPECT_FALSE(feeFreeSystem && seenOther) << "system account " << id << " renews after a crediting record";
      seenOther = seenOther || !feeFreeSystem;
    }
  }

  // Every system-account renewal in the set is fee-free (the full renewal
  // block needs a 3-key genesis account; smoke covers that path).
  for (const auto &r : renewals.value()) {
    auto renewal = utl::binaryUnpack<Ledger::TxRenewal>(r.data).value();
    // Genesis included: it issues the native token, so a fee would be minted.
    if (AccountIds::isSystemAccount(renewal.walletId)) {
      EXPECT_EQ(renewal.fee, 0u) << renewal.walletId;
    }
  }

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest, AddBlock_RejectsReplayOfAlreadyCommittedIndex) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-replay-old-index");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  ASSERT_TRUE(validator.addBlock(genesis).isOk());

  // block1 moves funds from RESERVE to FEE.
  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 100;
  tx.fee = 1;
  tx.idempotentId = 7;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  tx.meta.clear();
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  validator.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(validator, genesis, {rec});
  ASSERT_TRUE(validator.addBlock(block1).isOk());

  // Advance the tip past block1 with an unrelated non-empty block (heartbeat
  // policy would otherwise reject an empty block this soon after block1).
  Ledger::TxDefault tx2;
  tx2.tokenId = AccountBuffer::ID_GENESIS;
  tx2.fromWalletId = AccountBuffer::ID_RESERVE;
  tx2.toWalletId = AccountBuffer::ID_FEE;
  tx2.amount = 50;
  tx2.fee = 1;
  tx2.idempotentId = 8;
  tx2.validationTsMin = chainConfig.genesisTime;
  tx2.validationTsMax = chainConfig.genesisTime + 3600;
  tx2.meta.clear();
  Ledger::Record rec2 = makeRecord(Ledger::T_DEFAULT, tx2, reserveKey);

  validator.refreshStakeholders();
  Ledger::ChainNode block2 = makeNextBlock(validator, block1, {rec2});
  ASSERT_TRUE(validator.addBlock(block2).isOk());

  auto feeAfterBlock2 = validator.getAccount(AccountBuffer::ID_FEE);
  ASSERT_TRUE(feeAfterBlock2.isOk());
  const int64_t feeBalanceAfterBlock2 =
      feeAfterBlock2.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS);

  // Resubmitting the already-committed block1 (index 1, tip is now at 2)
  // must be rejected outright, not re-applied on top of the current tip.
  auto replayResult = validator.addBlock(block1);
  ASSERT_FALSE(replayResult.isOk());
  EXPECT_NE(replayResult.error().message.find("Invalid block index"),
            std::string::npos);

  // block1's RESERVE->FEE transfer must not have been double-applied.
  auto feeAfterReplay = validator.getAccount(AccountBuffer::ID_FEE);
  ASSERT_TRUE(feeAfterReplay.isOk());
  EXPECT_EQ(feeAfterReplay.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS),
            feeBalanceAfterBlock2);

  std::filesystem::remove_all(tempDir, ec);
}

TEST(ChainTest,
     ValidateIdempotencyRules_OnlyScansPreviousBlocksOnReplay) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 5;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-idempotency-replay");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());
  validator.setLedgerCommitPolicy(Ledger::CommitPolicy::everyBlock()); // replayed by a second chain below

  Ledger::ChainNode genesis = makeGenesisBlock(
      validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  auto addGenesisResult = validator.addBlock(genesis);
  ASSERT_TRUE(addGenesisResult.isOk());

  // Create a normal block with a single idempotent transaction from RESERVE to FEE.
  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = 0;
  tx.fee = 1; // Small non-zero fee; reserve account has ample balance.
  tx.idempotentId = 42;
  tx.validationTsMin = chainConfig.genesisTime;
  tx.validationTsMax = chainConfig.genesisTime + 3600;
  tx.meta.clear();
  Ledger::Record rec = makeRecord(Ledger::T_DEFAULT, tx, reserveKey);

  validator.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(validator, genesis, {rec});
  auto addBlock1Result = validator.addBlock(block1);
  ASSERT_TRUE(addBlock1Result.isOk());

  // Now create a new validator instance and replay from the existing ledger.
  Chain replayValidator;

  consensus::SlotCommittee::Config consensusConfig2;
  consensusConfig2.genesisTime = 0;
  consensusConfig2.timeOffset = 0;
  consensusConfig2.slotDuration = 5;
  consensusConfig2.slotsPerEpoch = 10;
  replayValidator.initConsensus(consensusConfig2);

  auto mountResult2 = replayValidator.mountLedger(tempDir.string());
  ASSERT_TRUE(mountResult2.isOk());

  auto loadResult = replayValidator.loadFromLedger(0);
  ASSERT_TRUE(loadResult.isOk())
      << "loadFromLedger failed: " << loadResult.error().message;
  EXPECT_EQ(loadResult.value(), 2u); // genesis + one normal block

  std::filesystem::remove_all(tempDir, ec);
}

// Reproduces late joiner scenario: config not set (no T_GENESIS/T_CONFIG processed),
// checkpoint.currentId == checkpoint.lastId. collectRenewals must return empty, not error.
TEST(ChainTest, LateJoiner_CollectRenewals_WhenConfigNotSet_ReturnsEmpty) {
  Chain validator;

  // Minimal consensus: late joiner init with timeOffset only (no genesis processed)
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 1000;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      pp::test::uniqueTempPath("pp-ledger-chain-test-late-joiner");
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  // Ledger starting from checkpoint (e.g. 5) - no blocks in ledger yet
  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 5;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  // loadFromLedger(5): empty ledger, processes nothing, txContext_.optChainConfig stays unset
  auto loadResult = validator.loadFromLedger(5);
  ASSERT_TRUE(loadResult.isOk());
  EXPECT_EQ(loadResult.value(), 5u);

  EXPECT_FALSE(validator.isChainConfigReady());

  // collectRenewals must not fail with "Chain config not initialized"
  auto renewalsResult = validator.collectRenewals(0);
  ASSERT_TRUE(renewalsResult.isOk())
      << "collectRenewals failed (late joiner config not set): "
      << (renewalsResult ? "" : renewalsResult.error().message);
  EXPECT_TRUE(renewalsResult.value().empty());

  std::filesystem::remove_all(tempDir, ec);
}


namespace {

struct ComposeHarness {
  Chain producer;
  Chain peer;
  Chain::BlockChainConfig chainConfig;
  utl::MlDsaKeyPair genesisKey;
  utl::MlDsaKeyPair feeKey;
  utl::MlDsaKeyPair reserveKey;
  utl::MlDsaKeyPair recycleKey;
  std::filesystem::path producerDir;
  std::filesystem::path peerDir;
  Ledger::ChainNode genesis;

  void SetUp(const std::function<void(Chain::BlockChainConfig &)> &tweakConfig = {}) {
    chainConfig = makeChainConfig(/*genesisTime=*/0);
    chainConfig.slotDuration = 5;
    chainConfig.slotsPerEpoch = 10;
    chainConfig.checkpoint.minBlocks = 100;
    chainConfig.checkpoint.minAgeSeconds = 0;
    chainConfig.maxIssuancePerEpoch = 100;
    chainConfig.newAccountFee = 5;
    chainConfig.minerMinStake = 50;
    chainConfig.minerRegistrationFee = 7;
    if (tweakConfig) {
      tweakConfig(chainConfig);
    }

    consensus::SlotCommittee::Config consensusConfig;
    consensusConfig.genesisTime = 0;
    consensusConfig.timeOffset = 0;
    consensusConfig.slotDuration = 5;
    consensusConfig.slotsPerEpoch = 10;
    producer.initConsensus(consensusConfig);
    peer.initConsensus(consensusConfig);

    genesisKey = makeKeyPair();
    feeKey = makeKeyPair();
    reserveKey = makeKeyPair();
    recycleKey = makeKeyPair();

    producerDir = pp::test::uniqueTempPath("pp-ledger-compose-producer");
    peerDir = pp::test::uniqueTempPath("pp-ledger-compose-peer");
    std::error_code ec;
    std::filesystem::remove_all(producerDir, ec);
    std::filesystem::remove_all(peerDir, ec);

    Ledger::InitConfig producerLedger;
    producerLedger.workDir = producerDir.string();
    producerLedger.startingBlockId = 0;
    ASSERT_TRUE(producer.initLedger(producerLedger).isOk());

    Ledger::InitConfig peerLedger;
    peerLedger.workDir = peerDir.string();
    peerLedger.startingBlockId = 0;
    ASSERT_TRUE(peer.initLedger(peerLedger).isOk());

    genesis = makeGenesisBlock(producer, chainConfig, genesisKey, feeKey,
                               reserveKey, recycleKey);
    ASSERT_TRUE(producer.addBlock(genesis).isOk());

    // Peer applies the same genesis bytes (unsealed path) so tip banks match.
    ASSERT_TRUE(peer.addBlock(genesis).isOk());

    producer.refreshStakeholders();
    peer.refreshStakeholders();
    ASSERT_FALSE(producer.getStakeholders().empty());
    ASSERT_EQ(producer.getStakeholders().size(), peer.getStakeholders().size());
  }

  void TearDown() {
    std::error_code ec;
    std::filesystem::remove_all(producerDir, ec);
    std::filesystem::remove_all(peerDir, ec);
  }
};

} // namespace

class ChainComposeTest : public ::testing::Test {
protected:
  void SetUp() override { harness_.SetUp(); }
  void TearDown() override { harness_.TearDown(); }
  ComposeHarness harness_;
};

// L-CONSENSUS-FORCE + L-SMOKE-L1 (in-process): forced leader block is accepted
// System accounts hold protocol funds (reserve holds nearly the whole supply)
// and never count as stake; the genesis miner is the committee.
TEST_F(ChainComposeTest, StakeExcludesSystemAccounts) {
  const auto stakeholders = harness_.producer.getStakeholders();
  ASSERT_EQ(stakeholders.size(), 1u);
  EXPECT_EQ(stakeholders.front().id, kTestMinerId);
  EXPECT_EQ(stakeholders.front().stake, static_cast<uint64_t>(kTestMinerStake));
}

// by producer (seal path) and by a peer (checkBlock Full path).
TEST_F(ChainComposeTest, ForcedLeader_ProducerAndPeerAcceptTip) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;

  // Empty body: use heartbeat threshold so seal/validate allow the empty.
  const uint64_t tipSlot = harness_.genesis.block.slot;
  const uint64_t slot = tipSlot + producer.getHeartbeatSlots();
  ASSERT_GE(slot, tipSlot + 1);
  const auto stakeholders = producer.getStakeholders();
  ASSERT_FALSE(stakeholders.empty());
  const uint64_t forced = stakeholders.front().id;

  producer.forceSlotLeader(slot, forced);
  peer.forceSlotLeader(slot, forced);
  // Pin clock inside the slot so timing checks on the peer unsealed path pass.
  producer.setClockOverride(producer.getSlotStartTime(slot));
  peer.setClockOverride(peer.getSlotStartTime(slot));

  Ledger::ChainNode block1 =
      makeNextBlockAtSlot(producer, harness_.genesis, slot, {});
  EXPECT_EQ(block1.block.slot, slot);
  EXPECT_EQ(block1.block.slotLeader, forced);
  ASSERT_TRUE(producer.addBlock(block1).isOk());

  // Peer did not seal — full checkBlock(Full) including slot-leader check.
  auto peerAdd = peer.addBlock(block1);
  ASSERT_TRUE(peerAdd.isOk()) << peerAdd.error().message;

  EXPECT_EQ(producer.getNextBlockId(), peer.getNextBlockId());
  auto tipP = producer.readLastBlock();
  auto tipQ = peer.readLastBlock();
  ASSERT_TRUE(tipP.isOk());
  ASSERT_TRUE(tipQ.isOk());
  EXPECT_EQ(tipP->hash, tipQ->hash);
  EXPECT_EQ(tipP->block.slotLeader, forced);
}

// L-CONSENSUS-WRONG-LEADER: unsealed addBlock rejects a forged slot leader.
TEST_F(ChainComposeTest, WrongLeader_UnsealedAddBlockRejected) {
  auto &producer = harness_.producer;

  // Slot at heartbeat threshold so empty-body policy does not fire first.
  const uint64_t tipSlot = harness_.genesis.block.slot;
  const uint64_t slot = tipSlot + producer.getHeartbeatSlots();
  const auto stakeholders = producer.getStakeholders();
  ASSERT_FALSE(stakeholders.empty());
  const uint64_t forced = stakeholders.front().id;
  const uint64_t wrong =
      (stakeholders.size() > 1) ? stakeholders.back().id : forced + 9999;
  ASSERT_NE(forced, wrong);

  producer.forceSlotLeader(slot, forced);
  producer.setClockOverride(producer.getSlotStartTime(slot));

  Ledger::ChainNode bad;
  bad.block.index = harness_.genesis.block.index + 1;
  bad.block.previousHash = harness_.genesis.hash;
  bad.block.slot = slot;
  bad.block.timestamp = producer.getSlotEndTime(slot);
  bad.block.slotLeader = wrong;
  bad.block.epoch = slot / 10; // slotsPerEpoch from harness config
  bad.block.txIndex =
      harness_.genesis.block.txIndex + harness_.genesis.block.records.size();
  bad.block.records = {};
  bad.block.txRoot = chain_block::calculateTxRoot(bad.block.records);
  bad.block.stakeSnapshotHash =
      chain_block::calculateStakeSnapshotHash(producer.getStakeholders());
  bad.block.committee = chain_block::toCommittee(producer.getStakeholders()); // it opens epoch 1
  ASSERT_TRUE(producer.ensureEpochSeed(bad.block.epoch).isOk());
  bad.block.epochSeed = producer.getEpochSeed();
  // Empty body: state unchanged; peer/producer tip root after genesis.
  // Hash must match header fields (including wrong leader) for validation to
  // reach the slot-leader check.
  auto tip = producer.readLastBlock();
  ASSERT_TRUE(tip.isOk());
  bad.block.stateRoot = tip->block.stateRoot;
  bad.hash = producer.calculateHash(bad.block);

  auto add = producer.addBlock(bad);
  ASSERT_TRUE(add.isError()) << "wrong leader must be rejected";
  // processNormalBlock maps checkBlock failures to E_BLOCK_VALIDATION.
  EXPECT_EQ(add.error().code, Chain::E_BLOCK_VALIDATION) << add.error().message;
  EXPECT_NE(add.error().message.find("slot leader"), std::string::npos)
      << add.error().message;
  EXPECT_EQ(producer.getNextBlockId(), harness_.genesis.block.index + 1);
}

namespace {

Ledger::Record makeReserveTransfer(const ComposeHarness &h, uint64_t amount,
                                   uint64_t idempotentId) {
  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_FEE;
  tx.amount = amount;
  tx.fee = 1;
  tx.idempotentId = idempotentId;
  tx.validationTsMin = h.chainConfig.genesisTime;
  tx.validationTsMax = h.chainConfig.genesisTime + 3600;
  return makeRecord(Ledger::T_DEFAULT, tx, h.reserveKey, h.chainConfig.networkId);
}

int64_t reserveBalance(const Chain &chain) {
  auto acc = chain.getAccount(AccountBuffer::ID_RESERVE);
  return acc ? acc->wallet.mBalances.at(AccountBuffer::ID_GENESIS) : -1;
}

} // namespace

// A block with valid txs but a forged stateRoot is rejected without touching
// tip state; the honest block for the same height is still accepted.
TEST_F(ChainComposeTest, BadStateRoot_RejectedThenHonestBlockAccepted) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t slot = harness_.genesis.block.slot + 1;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  producer.forceSlotLeader(slot, leaderId);
  peer.forceSlotLeader(slot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot));
  peer.setClockOverride(peer.getSlotStartTime(slot));

  Ledger::ChainNode honest = makeNextBlockAtSlot(
      producer, harness_.genesis, slot, {makeReserveTransfer(harness_, 100, 1)});
  ASSERT_EQ(honest.block.records.size(), 1u);

  Ledger::ChainNode forged = honest;
  forged.block.stateRoot = utl::sha256Raw("forged-state-root");
  forged.hash = peer.calculateHash(forged.block);

  const int64_t balanceBefore = reserveBalance(peer);
  const auto stakeBefore = peer.getStakeholders();
  auto bad = peer.addBlock(forged);
  ASSERT_TRUE(bad.isError());
  EXPECT_NE(bad.error().message.find("stateRoot"), std::string::npos)
      << bad.error().message;
  EXPECT_EQ(peer.getNextBlockId(), honest.block.index);
  EXPECT_EQ(reserveBalance(peer), balanceBefore);
  EXPECT_EQ(peer.getStakeholders().size(), stakeBefore.size());

  ASSERT_TRUE(producer.addBlock(honest).isOk());
  auto good = peer.addBlock(honest);
  ASSERT_TRUE(good.isOk()) << good.error().message;
  EXPECT_EQ(reserveBalance(peer), reserveBalance(producer));
  auto tipP = producer.readLastBlock();
  auto tipQ = peer.readLastBlock();
  ASSERT_TRUE(tipP.isOk());
  ASSERT_TRUE(tipQ.isOk());
  EXPECT_EQ(tipP->hash, tipQ->hash);
}

// A seal whose commit is rejected rolls the tip back; the producer can then
// seal and commit the next honest block.
TEST_F(ChainComposeTest, RejectedSeal_RollsBackAndProducerRecovers) {
  auto &producer = harness_.producer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  const uint64_t tipSlot = harness_.genesis.block.slot;
  const int64_t balanceBefore = reserveBalance(producer);

  // Reuses the tip slot: rejected either at seal or at commit.
  producer.forceSlotLeader(tipSlot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(tipSlot));
  auto bad = producer.linkNextBlock(harness_.genesis, tipSlot, leaderId,
                                    producer.getSlotEndTime(tipSlot),
                                    {makeReserveTransfer(harness_, 50, 1)});
  auto seal = producer.sealBlock(bad);
  if (seal.isOk()) {
    EXPECT_TRUE(producer.addBlock(bad).isError());
  }
  EXPECT_EQ(reserveBalance(producer), balanceBefore);
  EXPECT_EQ(producer.getNextBlockId(), harness_.genesis.block.index + 1);

  const uint64_t slot = tipSlot + 1;
  producer.forceSlotLeader(slot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot));
  Ledger::ChainNode next = makeNextBlockAtSlot(
      producer, harness_.genesis, slot, {makeReserveTransfer(harness_, 50, 1)});
  auto add = producer.addBlock(next);
  ASSERT_TRUE(add.isOk()) << add.error().message;
  EXPECT_EQ(reserveBalance(producer), balanceBefore - 51);
}

// Unsealed peer path applies the arrival checks before touching the tip.
TEST_F(ChainComposeTest, UnsealedAddBlock_RejectsSlotRegressionAndFarFuture) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  const uint64_t slot1 = harness_.genesis.block.slot + 2;
  producer.forceSlotLeader(slot1, leaderId);
  peer.forceSlotLeader(slot1, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot1));
  peer.setClockOverride(peer.getSlotStartTime(slot1));

  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, slot1, {makeReserveTransfer(harness_, 10, 1)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  ASSERT_TRUE(peer.addBlock(block1).isOk());

  const int64_t balanceBefore = reserveBalance(peer);
  auto relabel = [&](uint64_t slot) {
    Ledger::ChainNode forged = block1;
    forged.block.index = block1.block.index + 1;
    forged.block.previousHash = block1.hash;
    forged.block.slot = slot;
    forged.block.timestamp = peer.getSlotEndTime(slot);
    forged.hash = peer.calculateHash(forged.block);
    return forged;
  };

  auto regress = peer.addBlock(relabel(slot1));
  ASSERT_TRUE(regress.isError());
  EXPECT_NE(regress.error().message.find("Invalid block slot"),
            std::string::npos) << regress.error().message;

  // slotDuration 5 -> tolerance 3 slots.
  auto future = peer.addBlock(relabel(slot1 + 4));
  ASSERT_TRUE(future.isError());
  EXPECT_NE(future.error().message.find("too far in the future"),
            std::string::npos) << future.error().message;

  EXPECT_EQ(reserveBalance(peer), balanceBefore);
  EXPECT_EQ(peer.getNextBlockId(), block1.block.index + 1);
}

// A seal that is never committed (e.g. broadcast failed) can be abandoned;
// the tip is restored and the producer seals the next slot.
TEST_F(ChainComposeTest, AbandonSeal_RestoresTipAndAllowsNextSeal) {
  auto &producer = harness_.producer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  const uint64_t slot = harness_.genesis.block.slot + 1;
  producer.forceSlotLeader(slot, leaderId);
  producer.forceSlotLeader(slot + 1, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot));
  const int64_t balanceBefore = reserveBalance(producer);

  Ledger::ChainNode sealed = makeNextBlockAtSlot(
      producer, harness_.genesis, slot, {makeReserveTransfer(harness_, 20, 1)});
  EXPECT_EQ(reserveBalance(producer), balanceBefore - 21);

  producer.abandonSeal();
  EXPECT_EQ(reserveBalance(producer), balanceBefore);
  EXPECT_EQ(producer.getNextBlockId(), harness_.genesis.block.index + 1);

  producer.setClockOverride(producer.getSlotStartTime(slot + 1));
  Ledger::ChainNode next = makeNextBlockAtSlot(
      producer, harness_.genesis, slot + 1,
      {makeReserveTransfer(harness_, 20, 1)});
  auto add = producer.addBlock(next);
  ASSERT_TRUE(add.isOk()) << add.error().message;
  EXPECT_EQ(reserveBalance(producer), balanceBefore - 21);
  (void)sealed;
}

// Two leaders seal the same height; the peer's broadcast loses. Until the peer
// abandons its own seal it cannot take the winning block from sync (L1 smoke:
// a miner stuck forever after one failed broadcast).
TEST_F(ChainComposeTest, AbandonSeal_LetsLosingProducerAcceptWinningBlock) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  const uint64_t slot = harness_.genesis.block.slot + 1;
  producer.forceSlotLeader(slot, leaderId);
  peer.forceSlotLeader(slot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot));
  peer.setClockOverride(peer.getSlotStartTime(slot));

  Ledger::ChainNode winner = makeNextBlockAtSlot(
      producer, harness_.genesis, slot, {makeReserveTransfer(harness_, 100, 1)});
  Ledger::ChainNode loser = makeNextBlockAtSlot(
      peer, harness_.genesis, slot, {makeReserveTransfer(harness_, 50, 2)});
  ASSERT_EQ(loser.block.index, winner.block.index);
  ASSERT_NE(loser.hash, winner.hash);
  ASSERT_TRUE(producer.addBlock(winner).isOk());

  auto refused = peer.addBlock(winner);
  ASSERT_TRUE(refused.isError());
  EXPECT_NE(refused.error().message.find("uncommitted sealed block"), std::string::npos)
      << refused.error().message;

  peer.abandonSeal();
  auto accepted = peer.addBlock(winner);
  ASSERT_TRUE(accepted.isOk()) << accepted.error().message;
  EXPECT_EQ(reserveBalance(peer), reserveBalance(producer));
  auto tipP = producer.readLastBlock();
  auto tipQ = peer.readLastBlock();
  ASSERT_TRUE(tipP.isOk() && tipQ.isOk());
  EXPECT_EQ(tipP->hash, tipQ->hash);
}

// The stake snapshot for an epoch must depend only on the chain, never on when
// a node's wall-clock refresh ran. A node that refreshes into the next epoch and
// then receives a late block of the current epoch must still validate it with
// the same snapshot as the producer (L-SMOKE-LATEJOIN stakeSnapshotHash mismatch).
TEST_F(ChainComposeTest, StakeSnapshot_IndependentOfWallClockRefreshTiming) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  const uint64_t slotA = harness_.genesis.block.slot + 1;
  const uint64_t slotA2 = slotA + 1;
  ASSERT_EQ(producer.getEpochFromSlot(slotA), producer.getEpochFromSlot(slotA2));
  uint64_t slotNextEpoch = slotA2 + 1;
  while (producer.getEpochFromSlot(slotNextEpoch) == producer.getEpochFromSlot(slotA)) {
    ++slotNextEpoch;
  }
  for (uint64_t slot : {slotA, slotA2, slotNextEpoch}) {
    producer.forceSlotLeader(slot, leaderId);
    peer.forceSlotLeader(slot, leaderId);
  }

  // Producer: two stake-changing blocks in one epoch, then the next epoch's first.
  producer.setClockOverride(producer.getSlotStartTime(slotA));
  Ledger::ChainNode a = makeNextBlockAtSlot(producer, harness_.genesis, slotA,
                                            {makeReserveTransfer(harness_, 100, 1)});
  ASSERT_TRUE(producer.addBlock(a).isOk());
  producer.setClockOverride(producer.getSlotStartTime(slotA2));
  Ledger::ChainNode a2 = makeNextBlockAtSlot(producer, a, slotA2, {makeReserveTransfer(harness_, 50, 2)});
  ASSERT_TRUE(producer.addBlock(a2).isOk());
  producer.setClockOverride(producer.getSlotStartTime(slotNextEpoch));
  Ledger::ChainNode b = makeNextBlockAtSlot(producer, a2, slotNextEpoch,
                                            {makeReserveTransfer(harness_, 25, 3)});
  ASSERT_TRUE(producer.addBlock(b).isOk());

  // Peer: receives `a`, its clock crosses into the next epoch and its run loop
  // refreshes, and only then does the late `a2` arrive.
  peer.setClockOverride(peer.getSlotStartTime(slotA));
  ASSERT_TRUE(peer.addBlock(a).isOk());
  peer.setClockOverride(peer.getSlotStartTime(slotNextEpoch));
  peer.refreshStakeholders();
  auto lateA2 = peer.addBlock(a2);
  ASSERT_TRUE(lateA2.isOk()) << lateA2.error().message;
  peer.refreshStakeholders();
  auto addB = peer.addBlock(b);
  ASSERT_TRUE(addB.isOk()) << addB.error().message;
  auto tipP = producer.readLastBlock();
  auto tipQ = peer.readLastBlock();
  ASSERT_TRUE(tipP.isOk() && tipQ.isOk());
  EXPECT_EQ(tipP->hash, tipQ->hash);
}

// A lagging node's wall-clock refresh must not derive its tip epoch's seed
// from the clock epoch's provisional stakes. Seen in L-SMOKE-LATEJOIN: the
// wrong tip-epoch seed stuck (the clock epoch's seed could not be derived
// while the previous epoch had no local blocks) and every later block of the
// tip epoch was rejected with "Block epochSeed mismatch" until a restart.
TEST_F(ChainComposeTest, EpochSeed_LaggingRefreshDoesNotPoisonTipEpochSeed) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t leaderId = producer.getStakeholders().front().id;
  // Epoch T >= 1: epoch 0's seed comes from the genesis config, not stakes.
  uint64_t slotA = harness_.genesis.block.slot + 1;
  while (producer.getEpochFromSlot(slotA) == producer.getEpochFromSlot(harness_.genesis.block.slot)) {
    ++slotA;
  }
  const uint64_t slotA2 = slotA + 1;
  const uint64_t epochT = producer.getEpochFromSlot(slotA);
  ASSERT_GE(epochT, 1u);
  ASSERT_EQ(epochT, producer.getEpochFromSlot(slotA2));
  uint64_t slotT1 = slotA2 + 1;
  while (producer.getEpochFromSlot(slotT1) == epochT) {
    ++slotT1;
  }
  uint64_t slotT2 = slotT1 + 1;
  while (producer.getEpochFromSlot(slotT2) == epochT + 1) {
    ++slotT2;
  }
  for (uint64_t slot : {slotA, slotA2}) {
    producer.forceSlotLeader(slot, leaderId);
    peer.forceSlotLeader(slot, leaderId);
  }

  // Two stake-changing blocks in epoch T.
  producer.setClockOverride(producer.getSlotStartTime(slotA));
  Ledger::ChainNode a = makeNextBlockAtSlot(producer, harness_.genesis, slotA,
                                            {makeReserveTransfer(harness_, 100, 1)});
  ASSERT_TRUE(producer.addBlock(a).isOk());
  producer.setClockOverride(producer.getSlotStartTime(slotA2));
  Ledger::ChainNode a2 = makeNextBlockAtSlot(producer, a, slotA2, {makeReserveTransfer(harness_, 50, 2)});
  ASSERT_TRUE(producer.addBlock(a2).isOk());

  // Peer has `a`, then its run loop refreshes while it lags: once in epoch
  // T+1 (derives T+1's provisional seed), once in T+2 (T+1 has no local blocks).
  peer.setClockOverride(peer.getSlotStartTime(slotA));
  ASSERT_TRUE(peer.addBlock(a).isOk());
  peer.setClockOverride(peer.getSlotStartTime(slotT1));
  peer.refreshStakeholders();
  peer.setClockOverride(peer.getSlotStartTime(slotT2));
  peer.refreshStakeholders();

  auto lateA2 = peer.addBlock(a2);
  ASSERT_TRUE(lateA2.isOk()) << lateA2.error().message;
  auto tipP = producer.readLastBlock();
  auto tipQ = peer.readLastBlock();
  ASSERT_TRUE(tipP.isOk() && tipQ.isOk());
  EXPECT_EQ(tipP->hash, tipQ->hash);
}

TEST(ChainPolicyTest, ShouldSealEmptyHeartbeat) {
  EXPECT_FALSE(shouldSealEmptyHeartbeat(/*slot=*/10, /*tip=*/0, /*hb=*/0));
  EXPECT_FALSE(shouldSealEmptyHeartbeat(5, 0, 10));
  EXPECT_FALSE(shouldSealEmptyHeartbeat(9, 0, 10));
  EXPECT_TRUE(shouldSealEmptyHeartbeat(10, 0, 10));
  EXPECT_TRUE(shouldSealEmptyHeartbeat(15, 5, 10));
  EXPECT_FALSE(shouldSealEmptyHeartbeat(5, 10, 1)); // tip ahead of clock
}

TEST_F(ChainComposeTest, EmptyHeartbeat_ForcedLeaderSealAccepted) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  ASSERT_FALSE(producer.getStakeholders().empty());
  EXPECT_EQ(producer.getHeartbeatSlots(), 10u);

  const uint64_t tipSlot = harness_.genesis.block.slot;
  const uint64_t slot = tipSlot + producer.getHeartbeatSlots();
  const uint64_t leaderId = producer.getStakeholders().front().id;
  producer.forceSlotLeader(slot, leaderId);
  peer.forceSlotLeader(slot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(slot));
  peer.setClockOverride(peer.getSlotStartTime(slot));

  Ledger::ChainNode empty =
      makeNextBlockAtSlot(producer, harness_.genesis, slot, {});
  ASSERT_TRUE(empty.block.records.empty());
  ASSERT_TRUE(producer.addBlock(empty).isOk()) << "producer commit";
  ASSERT_TRUE(peer.addBlock(empty).isOk()) << "peer validate empty";
  EXPECT_EQ(producer.getNextBlockId(), empty.block.index + 1);
  EXPECT_EQ(peer.getNextBlockId(), empty.block.index + 1);
}

TEST_F(ChainComposeTest, EmptyHeartbeat_EarlyEmptyRejected) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  ASSERT_EQ(producer.getHeartbeatSlots(), 10u);

  const uint64_t tipSlot = harness_.genesis.block.slot;
  const uint64_t earlySlot = tipSlot + 1; // lag 1 < heartbeatSlots 10
  ASSERT_LT(earlySlot - tipSlot, producer.getHeartbeatSlots());
  const uint64_t leaderId = producer.getStakeholders().front().id;
  producer.forceSlotLeader(earlySlot, leaderId);
  peer.forceSlotLeader(earlySlot, leaderId);
  producer.setClockOverride(producer.getSlotStartTime(earlySlot));
  peer.setClockOverride(peer.getSlotStartTime(earlySlot));

  // Seal path must refuse premature empties.
  Ledger::ChainNode early;
  early.block.index = harness_.genesis.block.index + 1;
  early.block.previousHash = harness_.genesis.hash;
  early.block.slot = earlySlot;
  early.block.timestamp = producer.getSlotEndTime(earlySlot);
  early.block.slotLeader = leaderId;
  early.block.txIndex =
      harness_.genesis.block.txIndex + harness_.genesis.block.records.size();
  early.block.records = {};
  auto seal = producer.sealBlock(early);
  ASSERT_TRUE(seal.isError()) << "seal must reject premature empty";
  EXPECT_NE(seal.error().message.find("Empty heartbeat"), std::string::npos)
      << seal.error().message;

  // Peer unsealed path: forge a well-formed early empty (skip seal).
  early.block.epoch = earlySlot / 10;
  early.block.txRoot = chain_block::calculateTxRoot(early.block.records);
  early.block.stakeSnapshotHash =
      chain_block::calculateStakeSnapshotHash(peer.getStakeholders());
  ASSERT_TRUE(peer.ensureEpochSeed(early.block.epoch).isOk());
  early.block.epochSeed = peer.getEpochSeed();
  auto tip = peer.readLastBlock();
  ASSERT_TRUE(tip.isOk());
  early.block.stateRoot = tip->block.stateRoot;
  early.hash = peer.calculateHash(early.block);

  auto peerAdd = peer.addBlock(early);
  ASSERT_TRUE(peerAdd.isError()) << "peer must reject premature empty";
  EXPECT_EQ(peerAdd.error().code, Chain::E_BLOCK_VALIDATION)
      << peerAdd.error().message;
  EXPECT_NE(peerAdd.error().message.find("Empty heartbeat"), std::string::npos)
      << peerAdd.error().message;
  EXPECT_EQ(peer.getNextBlockId(), harness_.genesis.block.index + 1);
}


// An epoch without blocks does not stop the chain (WIRE_SCHEMA § Epoch seed): the next block's
// seed chains through the empty epoch with the empty-prev tip material, on the producer and on a
// peer admitting the block.
TEST_F(ChainComposeTest, EpochSeed_ChainsThroughEmptyEpochs) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t slot1 = 1;
  producer.setClockOverride(producer.getSlotStartTime(slot1));
  peer.setClockOverride(peer.getSlotStartTime(slot1));
  Ledger::ChainNode block1 = makeNextBlockAtSlot(producer, harness_.genesis, slot1, {makeReserveTransfer(harness_, 5, 50)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  ASSERT_TRUE(peer.addBlock(block1).isOk());

  // Epochs 1 and 2 (slots 10..29) pass without a block; the next one lands in epoch 3.
  const uint64_t slot3 = 3 * harness_.chainConfig.slotsPerEpoch + 2;
  producer.setClockOverride(producer.getSlotStartTime(slot3));
  peer.setClockOverride(peer.getSlotStartTime(slot3));
  Ledger::ChainNode block3 = makeNextBlockAtSlot(producer, block1, slot3, {makeReserveTransfer(harness_, 5, 51)});
  ASSERT_EQ(block3.block.epoch, 3u);
  ASSERT_TRUE(producer.addBlock(block3).isOk());
  auto peerAdd = peer.addBlock(block3);
  ASSERT_TRUE(peerAdd.isOk()) << peerAdd.error().message;

  // Spec oracle: epoch 1 from epoch 0's lookback; epochs 2 and 3 from an empty previous epoch.
  const std::string &networkId = harness_.chainConfig.networkId;
  const std::string stakeHash = chain_block::calculateStakeSnapshotHash(peer.getStakeholders());
  const std::string seed0 = harness_.genesis.block.epochSeed;
  const std::string seed1 = consensus::deriveEpochSeed(
      1, networkId, seed0, consensus::lookbackTipMaterial(0, seed0, {harness_.genesis.hash, block1.hash}), stakeHash);
  const std::string seed2 =
      consensus::deriveEpochSeed(2, networkId, seed1, consensus::emptyPrevEpochTipMaterial(1, seed1), stakeHash);
  const std::string seed3 =
      consensus::deriveEpochSeed(3, networkId, seed2, consensus::emptyPrevEpochTipMaterial(2, seed2), stakeHash);
  EXPECT_EQ(block3.block.epochSeed, seed3);

  // Bounded: at most kMaxEmptyEpochs empty epochs after the last block's epoch (3).
  EXPECT_TRUE(peer.ensureEpochSeed(3 + consensus::kMaxEmptyEpochs + 1).isOk());
  auto tooFar = peer.ensureEpochSeed(3 + consensus::kMaxEmptyEpochs + 2);
  ASSERT_FALSE(tooFar.isOk());
  EXPECT_NE(tooFar.error().message.find("empty epochs past"), std::string::npos) << tooFar.error().message;
}

// After genesis: genesis creates no accounts and transfers only to reserve
// (every genesis transfer mints); issued ids come only from the registrar;
// user ids from any funded account.
TEST_F(ChainComposeTest, Registrar_AloneCreatesIssuedAccounts_GenesisOnlyFundsReserve) {
  auto &producer = harness_.producer;
  const auto &registrarKey = harness_.recycleKey; // fixture default
  const int64_t now = static_cast<int64_t>(std::time(nullptr)); // buffer admission checks wall time
  uint64_t idem = 100;

  Ledger::TxDefault fund;
  fund.tokenId = AccountBuffer::ID_GENESIS;
  fund.fromWalletId = AccountBuffer::ID_RESERVE;
  fund.toWalletId = AccountBuffer::ID_REGISTRAR;
  fund.amount = 1000;
  fund.fee = 1;
  fund.idempotentId = idem++;
  fund.validationTsMin = harness_.chainConfig.genesisTime;
  fund.validationTsMax = harness_.chainConfig.genesisTime + 3600;
  Ledger::ChainNode block1 = makeNextBlock(producer, harness_.genesis,
                                           {makeRecord(Ledger::T_DEFAULT, fund, harness_.reserveKey)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  auto leader = producer.getSlotLeader(block1.block.slot + 1);
  ASSERT_TRUE(leader.isOk());

  auto newAccount = [&](uint64_t from, uint64_t to, const utl::MlDsaKeyPair &signer, uint64_t fee = 6) {
    Ledger::TxNewUser nu;
    nu.fromWalletId = from;
    nu.toWalletId = to;
    nu.amount = 10;
    nu.fee = fee; // meta fee 1 + newAccountFee 5
    nu.meta = makeUserAccount(makeKeyPair().publicKey, 10).ltsToString();
    nu.idempotentId = idem++;
    nu.validationTsMin = now - 60;
    nu.validationTsMax = now + 60;
    AccountBuffer scratch;
    return producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_NEW_USER, nu, signer, producer.getNetworkId()), leader.value());
  };
  auto expectRefused = [](const auto &added, const std::string &why) {
    ASSERT_FALSE(added.isOk()) << why;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  };

  const uint64_t issuedId = AccountBuffer::ID_FIRST_ISSUED + 1;
  const uint64_t userId = AccountBuffer::ID_FIRST_USER + 1;
  auto byRegistrar = newAccount(AccountBuffer::ID_REGISTRAR, issuedId, registrarKey);
  EXPECT_TRUE(byRegistrar.isOk()) << byRegistrar.error().message;
  auto byReserve = newAccount(AccountBuffer::ID_RESERVE, userId, harness_.reserveKey);
  EXPECT_TRUE(byReserve.isOk()) << byReserve.error().message;
  expectRefused(newAccount(AccountBuffer::ID_RESERVE, issuedId, harness_.reserveKey), "only by the registrar");
  expectRefused(newAccount(AccountBuffer::ID_GENESIS, userId, harness_.genesisKey), "Genesis creates no accounts");
  // Every creation pays the flat newAccountFee on top of the meta fee.
  expectRefused(newAccount(AccountBuffer::ID_RESERVE, userId + 1, harness_.reserveKey, 5), "newAccountFee 5");

  auto genesisTransfer = [&](uint64_t to) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = AccountBuffer::ID_GENESIS;
    tx.toWalletId = to;
    tx.amount = 10;
    tx.fee = 1;
    tx.idempotentId = idem++;
    tx.validationTsMin = now - 60;
    tx.validationTsMax = now + 60;
    AccountBuffer scratch;
    return producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_DEFAULT, tx, harness_.genesisKey, producer.getNetworkId()), leader.value());
  };
  auto toReserve = genesisTransfer(AccountBuffer::ID_RESERVE); // within maxIssuancePerEpoch
  EXPECT_TRUE(toReserve.isOk()) << toReserve.error().message;
  expectRefused(genesisTransfer(AccountBuffer::ID_FEE), "only to reserve");
  expectRefused(genesisTransfer(kTestMinerId), "only to reserve");
}

// A config update applies at the next epoch boundary, and only one can be
// pending: at most one takes effect per epoch. (Key rotation applies at once.)
TEST_F(ChainComposeTest, ConfigUpdate_AppliesAtNextEpochAndOnlyOneIsPending) {
  auto &producer = harness_.producer;
  const uint64_t before = producer.getMaxTransactionsPerBlock();
  const std::vector<utl::MlDsaKeyPair> rotated = {makeKeyPair(), makeKeyPair(), makeKeyPair()};

  auto configTx = [&](uint64_t maxTransactions, uint64_t idem, int64_t tsMin, int64_t tsMax) {
    Chain::GenesisAccountMeta gm;
    gm.config = harness_.chainConfig;
    gm.config.maxTransactionsPerBlock = maxTransactions;
    gm.genesis.wallet = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
    gm.genesis.wallet.publicKeys = {rotated[0].publicKey, rotated[1].publicKey, rotated[2].publicKey};
    gm.genesis.wallet.minSignatures = 2;
    gm.genesis.meta = AccountAttachment::emptySerialized();
    gm.authority = producer.getAuthority().value();
    Ledger::TxConfig tx;
    tx.meta = gm.ltsToString();
    tx.idempotentId = idem;
    tx.validationTsMin = tsMin;
    tx.validationTsMax = tsMax;
    return tx;
  };

  // Epoch 0 (slots 0..9): accepted, not yet in force.
  const auto first = configTx(before + 1, 1, harness_.chainConfig.genesisTime, harness_.chainConfig.genesisTime + 3600);
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1, {makeRecord(Ledger::T_CONFIG, first, harness_.genesisKey, producer.getNetworkId())});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  EXPECT_EQ(producer.getMaxTransactionsPerBlock(), before);

  // A second update in the same epoch is refused while the first is pending
  // (signed by the rotated keys, which are in force already).
  {
    Ledger::Record second;
    second.type = Ledger::T_CONFIG;
    second.data = utl::binaryPack(
        configTx(before + 2, 2, harness_.chainConfig.genesisTime, harness_.chainConfig.genesisTime + 3600));
    for (size_t i = 0; i < 2; ++i) {
      second.signatures.push_back(signMessage(rotated[i], second.signingMessage(producer.getNetworkId())));
    }
    ASSERT_TRUE(producer.ensureEpochSeed(0).isOk());
    auto leader = producer.getSlotLeader(2);
    ASSERT_TRUE(leader.isOk());
    auto block = producer.linkNextBlock(block1, 2, leader.value(), producer.getSlotEndTime(2), {second});
    auto sealed = producer.sealBlock(block);
    ASSERT_FALSE(sealed.isOk());
    EXPECT_NE(sealed.error().message.find("already pending"), std::string::npos) << sealed.error().message;
    producer.abandonSeal();
  }

  // First block of epoch 1: in force.
  Ledger::TxDefault transfer;
  transfer.tokenId = AccountBuffer::ID_GENESIS;
  transfer.fromWalletId = AccountBuffer::ID_RESERVE;
  transfer.toWalletId = AccountBuffer::ID_REGISTRAR;
  transfer.amount = 10;
  transfer.fee = 1;
  transfer.idempotentId = 1;
  transfer.validationTsMin = harness_.chainConfig.genesisTime;
  transfer.validationTsMax = harness_.chainConfig.genesisTime + 3600;
  Ledger::ChainNode block2 = makeNextBlockAtSlot(producer, block1, harness_.chainConfig.slotsPerEpoch,
                                                 {makeRecord(Ledger::T_DEFAULT, transfer, harness_.reserveKey)});
  ASSERT_TRUE(producer.addBlock(block2).isOk());
  EXPECT_EQ(producer.getMaxTransactionsPerBlock(), before + 1);
}

// Genesis issuance (to reserve, fee included) is capped per epoch by
// maxIssuancePerEpoch (100 in the harness); a new epoch starts afresh.
namespace {

/** Offers T_CONFIG updates (harness config + `change`, genesis rotated to 3 keys) to the
 *  producer's buffer, each with a fresh idempotency id. */
struct ConfigUpdateProbe {
  ComposeHarness &h;
  std::vector<utl::MlDsaKeyPair> rotated{makeKeyPair(), makeKeyPair(), makeKeyPair()};
  uint64_t idem = 1;

  explicit ConfigUpdateProbe(ComposeHarness &harness) : h(harness) {}

  Chain::Roe<void> add(const std::function<void(Chain::BlockChainConfig &)> &change) {
    return addMeta([&](Chain::GenesisAccountMeta &gm) { change(gm.config); });
  }

  /** A T_CONFIG whose meta (config, rotated genesis keys, authority in force) `tweak` edits. */
  Chain::Roe<void> addMeta(const std::function<void(Chain::GenesisAccountMeta &)> &tweak) {
    Chain::GenesisAccountMeta gm;
    gm.config = h.chainConfig;
    gm.genesis.wallet = h.producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
    gm.genesis.wallet.publicKeys = {rotated[0].publicKey, rotated[1].publicKey, rotated[2].publicKey};
    gm.genesis.wallet.minSignatures = 2;
    gm.genesis.meta = AccountAttachment::emptySerialized();
    gm.authority = h.producer.getAuthority().value();
    tweak(gm);
    Ledger::TxConfig tx;
    tx.meta = gm.ltsToString();
    tx.idempotentId = idem++;
    tx.validationTsMin = h.chainConfig.genesisTime;
    tx.validationTsMax = h.chainConfig.genesisTime + 3600;
    AccountBuffer scratch;
    auto added = h.producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_CONFIG, tx, h.genesisKey, h.producer.getNetworkId()), kTestMinerId);
    if (!added) {
      return added.error();
    }
    return {};
  }
};

} // namespace

// Slot timing is fixed at genesis (BLOCK_PIPELINE open item D): the slot schedule is set once
// from it, so a config update may not change slotDuration or slotsPerEpoch either way.
TEST_F(ChainComposeTest, ConfigUpdate_RefusesSlotTimingChanges) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  ConfigUpdateProbe probe(harness_);
  auto tryConfig = [&](const std::function<void(Chain::BlockChainConfig &)> &change) { return probe.add(change); };
  auto refused = [&](const std::function<void(Chain::BlockChainConfig &)> &change, const std::string &why) {
    auto added = tryConfig(change);
    ASSERT_FALSE(added.isOk()) << why;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  };
  refused([](auto &c) { c.slotDuration -= 1; }, "Slot duration is fixed at genesis");
  refused([](auto &c) { c.slotDuration += 1; }, "Slot duration is fixed at genesis");
  refused([](auto &c) { c.slotsPerEpoch += 1; }, "Slots per epoch is fixed at genesis");
  refused([](auto &c) { c.slotsPerEpoch -= 1; }, "Slots per epoch is fixed at genesis");
  auto other = tryConfig([](auto &c) { c.maxTransactionsPerBlock += 1; });
  EXPECT_TRUE(other.isOk()) << other.error().message;
}

namespace {

/** A T_BEACON_CERT for `cert`, signed by `signer` (the harness's admin key unless a test says otherwise). */
Ledger::Record makeBeaconCertRecord(const BeaconCertificate &cert, const utl::MlDsaKeyPair &signer,
                                    const std::string &networkId, uint64_t idem, int64_t genesisTime,
                                    uint64_t fee = 0) {
  Ledger::TxBeaconCert tx;
  tx.fee = fee;
  tx.meta = cert.ltsToString();
  tx.idempotentId = idem;
  tx.validationTsMin = genesisTime;
  tx.validationTsMax = genesisTime + 3600;
  return makeRecord(Ledger::T_BEACON_CERT, tx, signer, networkId);
}

/** The next certificate after `current`: a fresh key, cutover 0. */
BeaconCertificate nextCertificate(const Chain::ChainAuthority &current) {
  BeaconCertificate next = current.beacon;
  next.generation = current.beacon.generation + 1;
  next.publicKey = makeKeyPair().publicKey;
  next.cutoverHeight = 0;
  return next;
}

} // namespace

// beacon-authority ba2: the admin set certifies the next generation; it is in force once its
// block applies, and genesis renewals then carry it.
TEST_F(ChainComposeTest, BeaconCert_NextGenerationTakesEffect) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  const auto before = producer.getAuthority().value();
  const BeaconCertificate next = nextCertificate(before);
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeBeaconCertRecord(next, harness_.genesisKey, producer.getNetworkId(), 1, harness_.chainConfig.genesisTime)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  const auto after = producer.getAuthority().value();
  EXPECT_EQ(after.beacon.generation, 2u);
  EXPECT_EQ(after.beacon.publicKey, next.publicKey);
  EXPECT_TRUE(after.recovery.publicKeys == before.recovery.publicKeys) << "the recovery set is untouched";

  // A config update now carries generation 2 (and is refused with generation 1).
  ConfigUpdateProbe probe(harness_);
  EXPECT_TRUE(probe.addMeta([](auto &) {}).isOk());
  auto stale = probe.addMeta([&](auto &gm) { gm.authority = before; });
  ASSERT_FALSE(stale.isOk());
}

TEST_F(ChainComposeTest, BeaconCert_RefusesEveryBadCertificate) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  const auto current = producer.getAuthority().value();
  const auto admin = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
  uint64_t idem = 1;
  auto tryCert = [&](const BeaconCertificate &cert, const utl::MlDsaKeyPair &signer, uint64_t fee = 0) {
    AccountBuffer scratch;
    return producer.addBufferTransaction(
        scratch,
        makeBeaconCertRecord(cert, signer, producer.getNetworkId(), idem++, harness_.chainConfig.genesisTime, fee),
        kTestMinerId);
  };
  const std::vector<std::pair<std::string, std::function<void(BeaconCertificate &)>>> refused = {
      {"must be generation 2", [](auto &c) { c.generation = 1; }},
      {"must be generation 2", [](auto &c) { c.generation = 3; }},
      {"networkId does not match", [](auto &c) { c.networkId = "other-net"; }},
      {"must not be the current beacon key", [&](auto &c) { c.publicKey = current.beacon.publicKey; }},
      {"must not be the current beacon key", [&](auto &c) { c.publicKey = admin.publicKeys[0]; }},
      {"must not be the current beacon key", [&](auto &c) { c.publicKey = current.recovery.publicKeys[0]; }},
      {"malformed public key", [](auto &c) { c.publicKey = "short"; }},
  };
  for (const auto &[why, change] : refused) {
    BeaconCertificate cert = nextCertificate(current);
    change(cert);
    auto added = tryCert(cert, harness_.genesisKey);
    ASSERT_FALSE(added.isOk()) << why;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  }
  auto paid = tryCert(nextCertificate(current), harness_.genesisKey, 1);
  ASSERT_FALSE(paid.isOk());
  EXPECT_NE(paid.error().message.find("fee 0"), std::string::npos) << paid.error().message;
  auto unsignedByAdmin = tryCert(nextCertificate(current), harness_.reserveKey);
  EXPECT_FALSE(unsignedByAdmin.isOk()) << "only the admin set certifies a beacon";
  auto good = tryCert(nextCertificate(current), harness_.genesisKey);
  EXPECT_TRUE(good.isOk()) << good.error().message;
}

// beacon-authority ba5c: the previous generation never signs past a certificate's cutover, so a
// certificate commits no later than the block after it; a cutover above that (or none) is only a
// bound, and the one in force is the block before the commit — the last the old generation signed.
TEST_F(ChainComposeTest, BeaconCert_CommitsNoLaterThanTheBlockAfterItsCutover) {
  auto &producer = harness_.producer;
  const auto current = producer.getAuthority().value();
  const auto admin = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
  const std::string net = producer.getNetworkId();
  BeaconCertificate next = nextCertificate(current);
  auto commitAt = [&](uint64_t cutover, uint64_t blockId) {
    next.cutoverHeight = cutover;
    return chain_tx::validateNextBeaconCertificate(next, current, admin, net, blockId);
  };
  EXPECT_TRUE(commitAt(4, 5).isOk()) << "the block right after the cutover";
  EXPECT_TRUE(commitAt(9, 5).isOk()) << "a cutover not reached yet bounds nothing on chain";
  EXPECT_TRUE(commitAt(UINT64_MAX, 5).isOk()) << "no cutover: the old generation signs until the commit";
  auto passed = commitAt(3, 5);
  ASSERT_FALSE(passed.isOk()) << "block 4 was signed by the old generation, past its cutover";
  EXPECT_NE(passed.error().message.find("is passed"), std::string::npos) << passed.error().message;
  EXPECT_EQ(chain_tx::certificateInForce(next, 5).cutoverHeight, 4u);

  // On chain: no cutover in the record, the block before the commit in force.
  producer.setClockOverride(producer.getSlotStartTime(1));
  BeaconCertificate open = nextCertificate(current);
  open.cutoverHeight = UINT64_MAX;
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1, {makeBeaconCertRecord(open, harness_.genesisKey, net, 1, harness_.chainConfig.genesisTime)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  EXPECT_EQ(producer.getAuthority()->beacon.generation, 2u);
  EXPECT_EQ(producer.getAuthority()->beacon.cutoverHeight, 0u) << "generation 1 signed up to block 0";
}

// A draft validates against the chain's authority, which moves only when a block applies: a second
// certificate in the same draft would seal a block the chain refuses, so the draft refuses it.
TEST_F(ChainComposeTest, BeaconCert_OnePerBlock) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  const auto current = producer.getAuthority().value();
  AccountBuffer draft;
  const auto genesisTime = harness_.chainConfig.genesisTime;
  ASSERT_TRUE(producer
                  .addBufferTransaction(draft,
                                        makeBeaconCertRecord(nextCertificate(current), harness_.genesisKey,
                                                             producer.getNetworkId(), 1, genesisTime),
                                        kTestMinerId)
                  .isOk());
  auto second = producer.addBufferTransaction(
      draft, makeBeaconCertRecord(nextCertificate(current), harness_.genesisKey, producer.getNetworkId(), 2, genesisTime),
      kTestMinerId);
  ASSERT_FALSE(second.isOk());
  EXPECT_NE(second.error().message.find("at most one beacon certificate"), std::string::npos)
      << second.error().message;
}

// beacon-authority ba8: one certificate per block is a rule of the chain, not only of a producer's
// draft. A block certifying N+1 and then N+2 passes each certificate's own checks in turn; validators
// and the authority trail refuse it whole.
TEST_F(ChainComposeTest, BeaconCert_ABlockWithTwoCertificatesIsRefused) {
  auto &producer = harness_.producer;
  auto &peer = harness_.peer;
  const uint64_t slot = 1;
  producer.setClockOverride(producer.getSlotStartTime(slot));
  peer.setClockOverride(peer.getSlotStartTime(slot));
  const auto current = producer.getAuthority().value();
  const std::string net = producer.getNetworkId();
  const auto genesisTime = harness_.chainConfig.genesisTime;
  BeaconCertificate gen2 = nextCertificate(current);
  gen2.cutoverHeight = UINT64_MAX;
  BeaconCertificate gen3 = gen2;
  gen3.generation = 3;
  gen3.publicKey = makeKeyPair().publicKey;
  const std::vector<Ledger::Record> records = {makeBeaconCertRecord(gen2, harness_.genesisKey, net, 1, genesisTime),
                                               makeBeaconCertRecord(gen3, harness_.genesisKey, net, 2, genesisTime)};

  Ledger::Block shape;
  shape.records = records;
  auto counted = chain_block::validateAuthorityRecordCount(shape);
  ASSERT_FALSE(counted.isOk());
  EXPECT_NE(counted.error().message.find("at most one beacon certificate"), std::string::npos);

  // Sealed by a producer that skips the draft, the block still does not commit anywhere.
  producer.refreshStakeholders(slot);
  ASSERT_TRUE(producer.ensureEpochSeed(producer.getEpochFromSlot(slot)).isOk());
  auto leader = producer.getSlotLeader(slot);
  ASSERT_TRUE(leader.isOk());
  auto block = producer.linkNextBlock(harness_.genesis, slot, leader.value(), producer.getSlotEndTime(slot), records);
  ASSERT_TRUE(producer.sealBlock(block).isOk()) << "each certificate passes its own checks in turn";
  EXPECT_FALSE(producer.addBlock(block).isOk());
  EXPECT_EQ(producer.getAuthority()->beacon.generation, 1u);
  auto atPeer = peer.addBlock(block);
  ASSERT_FALSE(atPeer.isOk());
  EXPECT_NE(atPeer.error().message.find("at most one beacon certificate"), std::string::npos)
      << atPeer.error().message;
  EXPECT_EQ(peer.getAuthority()->beacon.generation, 1u);

  // A node following blocks or an authority proof refuses it too.
  Client::AuthorityProof proof;
  proof.steps = {{5, 0, records[0]}, {5, 0, records[1]}};
  auto replayed = AuthorityTrail::replayProof(harness_.genesis.block, proof, 5, 0);
  ASSERT_FALSE(replayed.isOk());
  EXPECT_NE(replayed.error().message.find("at most one beacon certificate"), std::string::npos)
      << replayed.error().message;
  proof.steps = {{5, 0, records[0]}, {6, 0, records[1]}};
  auto apart = AuthorityTrail::replayProof(harness_.genesis.block, proof, 6, 0);
  ASSERT_TRUE(apart.isOk()) << apart.error().message;
  EXPECT_EQ(apart.value().beacon().generation, 3u) << "one per block, in two blocks";
}

// Out of band (a peer, a file): any higher generation the admin set signed, for this chain.
namespace {

Client::SignedTip signTip(const Chain &chain, uint64_t height, uint64_t generation, const utl::MlDsaKeyPair &key) {
  Client::SignedTip tip;
  tip.networkId = chain.getNetworkId();
  tip.generation = generation;
  tip.height = height;
  tip.blockHash = chain.readBlock(height).value().hash;
  tip.signature = utl::mlDsaSign(key.privateKey, tip.signingMessage()).value();
  return tip;
}

} // namespace

// beacon-authority ba4: a node adopts a tip signed by the beacon generation in force for it.
TEST_F(ChainComposeTest, SignedTip_VerifiesOnlyWithTheGenerationInForce) {
  auto &producer = harness_.producer;
  const auto genesisTip = signTip(producer, 0, 1, testBeaconKey());
  EXPECT_TRUE(producer.verifySignedTip(genesisTip).isOk());

  auto otherNetwork = genesisTip;
  otherNetwork.networkId = "other-net";
  EXPECT_FALSE(producer.verifySignedTip(otherNetwork).isOk());
  EXPECT_FALSE(producer.verifySignedTip(signTip(producer, 0, 1, makeKeyPair())).isOk()) << "not the beacon key";
  auto altered = genesisTip;
  altered.height = 1;
  EXPECT_FALSE(producer.verifySignedTip(altered).isOk());

  // Generation 2 certified: its key now signs, generation 1 no longer does.
  producer.setClockOverride(producer.getSlotStartTime(1));
  const auto newKey = makeKeyPair();
  BeaconCertificate next = producer.getAuthority()->beacon;
  next.generation = 2;
  next.publicKey = newKey.publicKey;
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeBeaconCertRecord(next, harness_.genesisKey, producer.getNetworkId(), 1, harness_.chainConfig.genesisTime)});

  // beacon-authority ba5: a node that has not applied block 1 verifies a tip over it given the
  // block itself (the producer applied it when sealing).
  auto &peer = harness_.peer;
  Client::SignedTip pendingTip;
  pendingTip.networkId = producer.getNetworkId();
  pendingTip.generation = 2;
  pendingTip.height = 1;
  pendingTip.blockHash = block1.hash;
  pendingTip.signature = utl::mlDsaSign(newKey.privateKey, pendingTip.signingMessage()).value();
  EXPECT_FALSE(peer.verifySignedTip(pendingTip).isOk()) << "generation 2 is not in force yet";
  EXPECT_TRUE(peer.verifySignedTip(pendingTip, {&block1.block}).isOk());

  ASSERT_TRUE(producer.addBlock(block1).isOk());
  EXPECT_TRUE(producer.verifySignedTip(signTip(producer, 1, 2, newKey)).isOk());
  auto superseded = producer.verifySignedTip(signTip(producer, 1, 1, testBeaconKey()));
  ASSERT_FALSE(superseded.isOk());
  EXPECT_NE(superseded.error().message.find("superseded"), std::string::npos) << superseded.error().message;
}

// beacon-authority ba5b: a node holding the chain from genesis serves the authority-changing
// records up to a block; replayed from genesis they reach the authority in force there.
TEST_F(ChainComposeTest, AuthorityProof_ReplaysFromGenesisToTheAuthorityInForce) {
  auto &producer = harness_.producer;
  auto none = producer.authorityProof(0);
  ASSERT_TRUE(none.isOk()) << none.error().message;
  EXPECT_TRUE(none.value().steps.empty());
  auto atGenesis = AuthorityTrail::replayProof(harness_.genesis.block, none.value(), 0, 0);
  ASSERT_TRUE(atGenesis.isOk()) << atGenesis.error().message;
  EXPECT_TRUE(atGenesis.value().authority() == producer.getAuthority().value());

  producer.setClockOverride(producer.getSlotStartTime(1));
  BeaconCertificate next = producer.getAuthority()->beacon;
  next.generation = 2;
  next.publicKey = makeKeyPair().publicKey;
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeBeaconCertRecord(next, harness_.genesisKey, producer.getNetworkId(), 1, harness_.chainConfig.genesisTime)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());

  auto proof = producer.authorityProof(1);
  ASSERT_TRUE(proof.isOk()) << proof.error().message;
  ASSERT_EQ(proof.value().steps.size(), 1u);
  EXPECT_EQ(proof.value().steps[0].blockId, 1u);
  auto replayed = AuthorityTrail::replayProof(harness_.genesis.block, proof.value(), 1, 0);
  ASSERT_TRUE(replayed.isOk()) << replayed.error().message;
  EXPECT_TRUE(replayed.value().authority() == producer.getAuthority().value()) << "generation 2, as in force";
  EXPECT_TRUE(replayed.value().agrees(producer.getAuthority().value(),
                                      producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet));

  EXPECT_FALSE(AuthorityTrail::replayProof(harness_.genesis.block, proof.value(), 0, 0).isOk())
      << "a step beyond the block";
  EXPECT_FALSE(producer.authorityProof(producer.getNextBlockId()).isOk()) << "beyond the tip";
  auto withheld = AuthorityTrail::replayProof(harness_.genesis.block, Client::AuthorityProof{}, 1, 0);
  ASSERT_TRUE(withheld.isOk());
  EXPECT_FALSE(withheld.value().authority() == producer.getAuthority().value()) << "a withheld step reaches another authority";
  EXPECT_FALSE(withheld.value()
                   .agrees(producer.getAuthority().value(), producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet)
                   .isOk());
}

// The proof follows admin rotations: a certificate signed by the admins a config update installed
// replays; one signed by the replaced admins, or by anyone else, does not.
TEST_F(ChainComposeTest, AuthorityProof_FollowsAdminRotation) {
  const std::string net = harness_.producer.getNetworkId();
  std::vector<utl::MlDsaKeyPair> admins{makeKeyPair(), makeKeyPair(), makeKeyPair()};
  Chain::GenesisAccountMeta gm;
  gm.config = harness_.chainConfig;
  gm.genesis.wallet = harness_.producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
  gm.genesis.wallet.publicKeys = {admins[0].publicKey, admins[1].publicKey, admins[2].publicKey};
  gm.genesis.wallet.minSignatures = 2;
  gm.authority = harness_.producer.getAuthority().value();
  Ledger::TxConfig config;
  config.meta = gm.ltsToString();
  const auto rotateAdmins = makeRecord(Ledger::T_CONFIG, config, harness_.genesisKey, net);

  BeaconCertificate next = gm.authority.beacon;
  next.generation = 2;
  next.publicKey = makeKeyPair().publicKey;
  next.cutoverHeight = UINT64_MAX; // none: generation 1 signs until block 7 commits it
  auto certBy = [&](const std::vector<utl::MlDsaKeyPair> &signers) {
    auto record = makeBeaconCertRecord(next, signers[0], net, 0, harness_.chainConfig.genesisTime);
    for (size_t i = 1; i < signers.size(); ++i) {
      record.signatures.push_back(signMessage(signers[i], record.signingMessage(net)));
    }
    return record;
  };
  auto replay = [&](const Ledger::Record &cert) {
    Client::AuthorityProof proof;
    proof.steps = {{3, 0, rotateAdmins}, {7, 0, cert}};
    return AuthorityTrail::replayProof(harness_.genesis.block, proof, 9, 0);
  };
  auto rotated = replay(certBy({admins[0], admins[2]}));
  ASSERT_TRUE(rotated.isOk()) << rotated.error().message;
  EXPECT_EQ(rotated.value().beacon().generation, 2u);
  EXPECT_EQ(rotated.value().beacon().publicKey, next.publicKey);
  EXPECT_EQ(rotated.value().beacon().cutoverHeight, 6u) << "in force: the block before its commit";
  EXPECT_EQ(rotated.value().admin().publicKeys, gm.genesis.wallet.publicKeys) << "the admins the config installed";
  EXPECT_FALSE(replay(certBy({harness_.genesisKey})).isOk()) << "the replaced admin set";
  EXPECT_FALSE(replay(certBy({makeKeyPair(), makeKeyPair()})).isOk()) << "not an admin set";

  Client::AuthorityProof outOfOrder;
  outOfOrder.steps = {{7, 0, certBy({admins[0], admins[1]})}, {3, 0, rotateAdmins}};
  EXPECT_FALSE(AuthorityTrail::replayProof(harness_.genesis.block, outOfOrder, 9, 0).isOk());
}

TEST_F(ChainComposeTest, BeaconCert_VerifyRecordOutOfBand) {
  auto &producer = harness_.producer;
  const auto current = producer.getAuthority().value();
  const auto genesisTime = harness_.chainConfig.genesisTime;
  BeaconCertificate skip = nextCertificate(current);
  skip.generation = 3;
  auto verified = producer.verifyBeaconCertificateRecord(
      makeBeaconCertRecord(skip, harness_.genesisKey, producer.getNetworkId(), 1, genesisTime));
  ASSERT_TRUE(verified.isOk()) << verified.error().message;
  EXPECT_EQ(verified.value().generation, 3u);

  BeaconCertificate stale = nextCertificate(current);
  stale.generation = 1;
  EXPECT_FALSE(producer
                   .verifyBeaconCertificateRecord(
                       makeBeaconCertRecord(stale, harness_.genesisKey, producer.getNetworkId(), 2, genesisTime))
                   .isOk());
  EXPECT_FALSE(producer
                   .verifyBeaconCertificateRecord(makeBeaconCertRecord(nextCertificate(current), harness_.reserveKey,
                                                                       producer.getNetworkId(), 3, genesisTime))
                   .isOk())
      << "not signed by the admin set";
}

// beacon-authority ba5c: a node keeps the highest certificate it sees out of band; until a block
// commits that generation, tips of older generations above its cutover are refused (whoever signed
// them — the case of a stolen beacon key), and the beacon commits nothing above it but a certificate.
TEST_F(ChainComposeTest, BeaconCert_SeenOutOfBandBoundsOlderTipsUntilOnChain) {
  auto &producer = harness_.producer;
  const auto current = producer.getAuthority().value();
  const std::string net = producer.getNetworkId();
  const auto genesisTime = harness_.chainConfig.genesisTime;
  uint64_t idem = 1;
  auto certRecord = [&](uint64_t generation, uint64_t cutover, const utl::MlDsaKeyPair &signer) {
    BeaconCertificate cert = nextCertificate(current);
    cert.generation = generation;
    cert.cutoverHeight = cutover;
    return makeBeaconCertRecord(cert, signer, net, idem++, genesisTime);
  };
  auto tipAt = [&](uint64_t height) {
    Client::SignedTip tip;
    tip.networkId = net;
    tip.generation = 1;
    tip.height = height;
    tip.blockHash = "block-" + std::to_string(height);
    tip.signature = utl::mlDsaSign(testBeaconKey().privateKey, tip.signingMessage()).value();
    return tip;
  };
  using Offer = Chain::CertificateOffer;
  EXPECT_FALSE(producer.currentCertificateRecord().has_value()) << "generation 1 has no record";
  EXPECT_TRUE(producer.verifySignedTip(tipAt(4)).isOk());

  const auto gen3 = certRecord(3, 3, harness_.genesisKey);
  ASSERT_EQ(producer.offerCertificate(gen3).value(), Offer::Newer);
  EXPECT_EQ(producer.offerCertificate(gen3).value(), Offer::Same);
  EXPECT_EQ(producer.offerCertificate(certRecord(3, 3, harness_.genesisKey)).value(), Offer::Conflict)
      << "another key for the generation held";
  EXPECT_EQ(producer.offerCertificate(certRecord(2, 3, harness_.genesisKey)).value(), Offer::Older);
  EXPECT_EQ(producer.offerCertificate(certRecord(1, 3, harness_.genesisKey)).value(), Offer::Older);
  EXPECT_FALSE(producer.offerCertificate(certRecord(4, 3, harness_.reserveKey)).isOk()) << "not admin-signed";
  ASSERT_TRUE(producer.seenCertificate().has_value());
  EXPECT_EQ(producer.seenCertificate()->generation, 3u);
  ASSERT_TRUE(producer.currentCertificateRecord().has_value());
  EXPECT_EQ(producer.currentCertificateRecord()->data, gen3.data) << "STATUS serves the one held";

  EXPECT_TRUE(producer.verifySignedTip(tipAt(3)).isOk()) << "at the cutover";
  auto past = producer.verifySignedTip(tipAt(4));
  ASSERT_FALSE(past.isOk()) << "above the cutover";
  EXPECT_NE(past.error().message.find("past block 3"), std::string::npos) << past.error().message;

  Ledger::Block plain;
  plain.index = 3;
  EXPECT_TRUE(producer.checkSeenCutover(plain).isOk());
  plain.index = 4;
  EXPECT_FALSE(producer.checkSeenCutover(plain).isOk()) << "the beacon commits nothing past the cutover";
  plain.records.push_back(certRecord(2, 3, harness_.genesisKey));
  EXPECT_TRUE(producer.checkSeenCutover(plain).isOk()) << "but a certificate";

  // Generation 2 on chain: generation 3 is still above it and stays held.
  producer.setClockOverride(producer.getSlotStartTime(1));
  Ledger::ChainNode block1 =
      makeNextBlockAtSlot(producer, harness_.genesis, 1, {certRecord(2, UINT64_MAX, harness_.genesisKey)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  ASSERT_TRUE(producer.seenCertificate().has_value());
  EXPECT_EQ(producer.seenCertificate()->generation, 3u);

  // Generation 3 on chain: the one held is dropped, and the tips rule falls back to the chain.
  producer.setClockOverride(producer.getSlotStartTime(2));
  Ledger::ChainNode block2 = makeNextBlockAtSlot(producer, block1, 2, {gen3});
  ASSERT_TRUE(producer.addBlock(block2).isOk());
  EXPECT_EQ(producer.getAuthority()->beacon.generation, 3u);
  EXPECT_FALSE(producer.seenCertificate().has_value());
  ASSERT_TRUE(producer.currentCertificateRecord().has_value());
  EXPECT_EQ(producer.currentCertificateRecord()->data, gen3.data) << "the latest on chain";
  EXPECT_EQ(producer.offerCertificate(gen3).value(), Offer::Older);
}

// beacon-authority ba6 (B005): two tips of one generation and height naming different blocks are
// proof the beacon key signed twice. A node holding the evidence verifies no tip of that generation
// above its height — it commits nothing past it — until a later generation is certified, out of
// band or on chain.
TEST_F(ChainComposeTest, BeaconEquivocation_HaltsAboveItsHeightUntilALaterGeneration) {
  auto &producer = harness_.producer;
  const auto current = producer.getAuthority().value();
  const std::string net = producer.getNetworkId();
  auto tipBy = [&](const utl::MlDsaKeyPair &key, uint64_t generation, uint64_t height, const std::string &hash) {
    Client::SignedTip tip;
    tip.networkId = net;
    tip.generation = generation;
    tip.height = height;
    tip.blockHash = hash;
    tip.signature = utl::mlDsaSign(key.privateKey, tip.signingMessage()).value();
    return tip;
  };
  auto tipAt = [&](uint64_t height) { return tipBy(testBeaconKey(), 1, height, "block-" + std::to_string(height)); };
  using Offer = Chain::EquivocationOffer;

  // Shape: one network, generation and height, different blocks; a re-signed block is no evidence.
  const auto a = tipBy(testBeaconKey(), 1, 3, "block-3");
  EXPECT_FALSE(Client::Equivocation::of(a, tipBy(testBeaconKey(), 1, 3, "block-3")).has_value());
  EXPECT_FALSE(Client::Equivocation::of(a, tipBy(testBeaconKey(), 1, 4, "fork-3")).has_value());
  const auto fork = tipBy(testBeaconKey(), 1, 3, "fork-3");
  const auto evidence = Client::Equivocation::of(fork, a);
  ASSERT_TRUE(evidence.has_value());
  EXPECT_TRUE(Client::Equivocation::of(a, fork) == evidence) << "canonical order";

  // Refused: not the beacon's key; a generation no certificate here names.
  EXPECT_FALSE(producer.offerEquivocation(*Client::Equivocation::of(a, tipBy(makeKeyPair(), 1, 3, "fork-3"))).isOk());
  const auto gen2Key = makeKeyPair();
  auto unknown = producer.offerEquivocation(
      *Client::Equivocation::of(tipBy(gen2Key, 2, 3, "x"), tipBy(gen2Key, 2, 3, "y")));
  ASSERT_FALSE(unknown.isOk());
  EXPECT_NE(unknown.error().message.find("no certificate here names"), std::string::npos) << unknown.error().message;
  EXPECT_FALSE(producer.equivocation().has_value());

  // Kept: tips of generation 1 above block 3 no longer verify.
  EXPECT_TRUE(producer.verifySignedTip(tipAt(4)).isOk());
  ASSERT_EQ(producer.offerEquivocation(*evidence).value(), Offer::Newer);
  EXPECT_EQ(producer.offerEquivocation(*evidence).value(), Offer::Same);
  EXPECT_EQ(producer.offerEquivocation(*Client::Equivocation::of(tipBy(testBeaconKey(), 1, 2, "x"),
                                                                  tipBy(testBeaconKey(), 1, 2, "y")))
                .value(),
            Offer::Ignored)
      << "one piece of evidence per generation";
  EXPECT_TRUE(producer.verifySignedTip(tipAt(3)).isOk()) << "at its height";
  auto halted = producer.verifySignedTip(tipAt(4));
  ASSERT_FALSE(halted.isOk());
  EXPECT_NE(halted.error().message.find("equivocation"), std::string::npos) << halted.error().message;

  // Resume out of band: a certificate of generation 2 supersedes it.
  BeaconCertificate next = nextCertificate(current);
  next.publicKey = gen2Key.publicKey;
  next.cutoverHeight = 5;
  ASSERT_EQ(producer.offerCertificate(makeBeaconCertRecord(next, harness_.genesisKey, net, 1,
                                                           harness_.chainConfig.genesisTime))
                .value(),
            Chain::CertificateOffer::Newer);
  EXPECT_FALSE(producer.equivocation().has_value()) << "resumed";
  EXPECT_TRUE(producer.verifySignedTip(tipAt(4)).isOk()) << "generation 1 up to the new cutover";
  EXPECT_EQ(producer.offerEquivocation(*evidence).value(), Offer::Ignored) << "superseded";

  // Evidence against the generation seen out of band verifies with its key.
  const auto gen2Evidence = Client::Equivocation::of(tipBy(gen2Key, 2, 6, "x"), tipBy(gen2Key, 2, 6, "y"));
  ASSERT_EQ(producer.offerEquivocation(*gen2Evidence).value(), Offer::Newer);
}

// ba6: committing a later certificate on chain resumes too.
TEST_F(ChainComposeTest, BeaconEquivocation_ResumesWhenALaterCertificateCommits) {
  auto &producer = harness_.producer;
  const std::string net = producer.getNetworkId();
  auto tipAt = [&](const std::string &hash) {
    Client::SignedTip tip;
    tip.networkId = net;
    tip.generation = 1;
    tip.height = 0;
    tip.blockHash = hash;
    tip.signature = utl::mlDsaSign(testBeaconKey().privateKey, tip.signingMessage()).value();
    return tip;
  };
  ASSERT_EQ(producer.offerEquivocation(*Client::Equivocation::of(tipAt("x"), tipAt("y"))).value(),
            Chain::EquivocationOffer::Newer);
  producer.setClockOverride(producer.getSlotStartTime(1));
  BeaconCertificate next = nextCertificate(producer.getAuthority().value());
  next.cutoverHeight = UINT64_MAX;
  Ledger::ChainNode block1 = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeBeaconCertRecord(next, harness_.genesisKey, net, 1, harness_.chainConfig.genesisTime)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  EXPECT_EQ(producer.getAuthority()->beacon.generation, 2u);
  EXPECT_FALSE(producer.equivocation().has_value());
}

namespace {

/** A T_RECOVERY naming `admins` (threshold `minSignatures`), signed by `signer`. */
Ledger::Record makeRecoveryRecord(const std::vector<utl::MlDsaKeyPair> &admins, uint8_t minSignatures,
                                  const utl::MlDsaKeyPair &signer, const std::string &networkId, uint64_t idem,
                                  int64_t genesisTime) {
  RecoveryAnnouncement announcement;
  announcement.networkId = networkId;
  for (const auto &admin : admins) {
    announcement.adminPublicKeys.push_back(admin.publicKey);
  }
  announcement.adminMinSignatures = minSignatures;
  Ledger::TxRecovery tx;
  tx.meta = announcement.ltsToString();
  tx.idempotentId = idem;
  tx.validationTsMin = genesisTime;
  tx.validationTsMax = genesisTime + 3600;
  return makeRecord(Ledger::T_RECOVERY, tx, signer, networkId);
}

} // namespace

// beacon-authority ba7 (B004 / B007): the recovery set announces a new admin set. While pending the
// admin set is frozen — no config update, no certificate, on chain or out of band — and the first
// block of the epoch `delayEpochs` later replaces it in place; from then on only the new admins sign.
TEST_F(ChainComposeTest, Recovery_FreezesTheAdminSetThenReplacesIt) {
  auto &producer = harness_.producer;
  const std::string net = producer.getNetworkId();
  const auto genesisTime = harness_.chainConfig.genesisTime;
  const auto oldKeys = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet.publicKeys;
  const std::vector<utl::MlDsaKeyPair> admins = {makeKeyPair(), makeKeyPair(), makeKeyPair()};
  ASSERT_EQ(producer.getAuthority()->recovery.delayEpochs, 3u);
  producer.setClockOverride(producer.getSlotStartTime(1));
  auto tryAdd = [&](const Ledger::Record &record) {
    AccountBuffer scratch;
    return producer.addBufferTransaction(scratch, record, kTestMinerId);
  };
  auto refusedWith = [&](const Ledger::Record &record, const std::string &why) {
    auto added = tryAdd(record);
    ASSERT_FALSE(added.isOk()) << why;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  };

  // Only the recovery set announces; the new set must have the genesis shape.
  EXPECT_FALSE(tryAdd(makeRecoveryRecord(admins, 2, harness_.genesisKey, net, 1, genesisTime)).isOk())
      << "the admin set cannot announce";
  refusedWith(makeRecoveryRecord({admins[0], admins[1]}, 2, testRecoveryKey(), net, 2, genesisTime),
              "between 3 and");
  refusedWith(makeRecoveryRecord(admins, 1, testRecoveryKey(), net, 3, genesisTime), "minSignatures");

  // Announced in epoch 0: pending until epoch 3.
  Ledger::ChainNode block =
      makeNextBlockAtSlot(producer, harness_.genesis, 1, {makeRecoveryRecord(admins, 2, testRecoveryKey(), net, 4, genesisTime)});
  ASSERT_TRUE(producer.addBlock(block).isOk());
  ASSERT_TRUE(producer.getAuthority()->hasPendingRecovery);
  EXPECT_EQ(producer.getAuthority()->pendingRecovery.applyEpoch, 3u);
  EXPECT_EQ(producer.getAuthority()->pendingRecovery.announcedAt, 1u);
  EXPECT_EQ(producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet.publicKeys, oldKeys) << "not yet";
  ASSERT_TRUE(producer.recoveryStatus().has_value()) << "STATUS shows it";
  EXPECT_EQ(producer.recoveryStatus()->applyEpoch, 3u);
  EXPECT_EQ(producer.recoveryStatus()->adminPublicKeys.size(), 3u);

  // Frozen: a second announcement, a config update, a certificate in a block or out of band.
  producer.setClockOverride(producer.getSlotStartTime(2));
  refusedWith(makeRecoveryRecord(admins, 2, testRecoveryKey(), net, 5, genesisTime), "already pending");
  ConfigUpdateProbe probe(harness_);
  auto config = probe.add([](auto &c) { c.maxTransactionsPerBlock += 1; });
  ASSERT_FALSE(config.isOk());
  EXPECT_NE(config.error().message.find("freezes the admin set"), std::string::npos) << config.error().message;
  const auto cert = makeBeaconCertRecord(nextCertificate(producer.getAuthority().value()), harness_.genesisKey, net,
                                         6, genesisTime);
  refusedWith(cert, "freezes the admin set");
  EXPECT_FALSE(producer.offerCertificate(cert).isOk()) << "out of band too";

  // The first block of epoch 3 replaces the admin set before its records.
  producer.setClockOverride(producer.getSlotStartTime(30));
  block = makeNextBlockAtSlot(producer, block, 30, {makeReserveTransfer(harness_, 5, 7)});
  ASSERT_TRUE(producer.addBlock(block).isOk());
  EXPECT_FALSE(producer.getAuthority()->hasPendingRecovery);
  EXPECT_FALSE(producer.recoveryStatus().has_value());
  const auto admin = producer.getAccount(AccountBuffer::ID_GENESIS).value();
  ASSERT_EQ(admin.wallet.publicKeys.size(), 3u);
  EXPECT_EQ(admin.wallet.publicKeys[0], admins[0].publicKey);
  EXPECT_EQ(admin.wallet.minSignatures, 2u);

  // The old admin key signs nothing now; two of the new ones do.
  producer.setClockOverride(producer.getSlotStartTime(31));
  BeaconCertificate rotation = nextCertificate(producer.getAuthority().value());
  rotation.cutoverHeight = UINT64_MAX; // none
  auto byOldAdmin = tryAdd(makeBeaconCertRecord(rotation, harness_.genesisKey, net, 8, genesisTime));
  ASSERT_FALSE(byOldAdmin.isOk());
  EXPECT_NE(byOldAdmin.error().message.find("signature"), std::string::npos) << byOldAdmin.error().message;
  auto byNewAdmins = makeBeaconCertRecord(rotation, admins[0], net, 9, genesisTime);
  byNewAdmins.signatures.push_back(signMessage(admins[2], byNewAdmins.signingMessage(net)));
  auto accepted = tryAdd(byNewAdmins);
  EXPECT_TRUE(accepted.isOk()) << accepted.error().message;

  // A replay of the authority proof reaches the same: pending before epoch 3, replaced from it.
  auto proof = producer.authorityProof(block.block.index);
  ASSERT_TRUE(proof.isOk()) << proof.error().message;
  ASSERT_EQ(proof.value().steps.size(), 1u);
  auto before = AuthorityTrail::replayProof(harness_.genesis.block, proof.value(), 1, 2);
  ASSERT_TRUE(before.isOk()) << before.error().message;
  EXPECT_TRUE(before.value().authority().hasPendingRecovery);
  EXPECT_EQ(before.value().admin().publicKeys, oldKeys);
  auto after = AuthorityTrail::replayProof(harness_.genesis.block, proof.value(), block.block.index, 3);
  ASSERT_TRUE(after.isOk()) << after.error().message;
  EXPECT_TRUE(after.value().agrees(producer.getAuthority().value(), admin.wallet).isOk());
}

// beacon-authority bp1 (B023): a chain holding blocks above the cutover of a certificate it keeps
// out of band is ordered to rewind to the cutover; at or below it, nothing is ordered.
TEST_F(ChainComposeTest, BeaconCert_ACutoverBelowTheTipOrdersARewind) {
  auto &producer = harness_.producer;
  const auto current = producer.getAuthority().value();
  const std::string net = producer.getNetworkId();
  const auto genesisTime = harness_.chainConfig.genesisTime;
  producer.setClockOverride(producer.getSlotStartTime(1));
  BeaconCertificate gen2 = nextCertificate(current);
  gen2.cutoverHeight = UINT64_MAX;
  Ledger::ChainNode block1 = makeNextBlockAtSlot(producer, harness_.genesis, 1,
                                                 {makeBeaconCertRecord(gen2, harness_.genesisKey, net, 1, genesisTime)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  EXPECT_FALSE(producer.rewindOrder().has_value()) << "no certificate held out of band";

  BeaconCertificate gen3 = gen2;
  gen3.generation = 3;
  gen3.publicKey = makeKeyPair().publicKey;
  gen3.cutoverHeight = 1;
  ASSERT_EQ(producer.offerCertificate(makeBeaconCertRecord(gen3, harness_.genesisKey, net, 2, genesisTime)).value(),
            Chain::CertificateOffer::Newer);
  EXPECT_FALSE(producer.rewindOrder().has_value()) << "the tip is at the cutover";

  gen3.publicKey = makeKeyPair().publicKey;
  gen3.generation = 4;
  gen3.cutoverHeight = 0; // block 1 is past it
  ASSERT_EQ(producer.offerCertificate(makeBeaconCertRecord(gen3, harness_.genesisKey, net, 3, genesisTime)).value(),
            Chain::CertificateOffer::Newer);
  auto order = producer.rewindOrder();
  ASSERT_TRUE(order.has_value());
  EXPECT_EQ(order->keptUpTo, 0u);
  EXPECT_EQ(order->droppedUpTo, 1u);
  EXPECT_EQ(order->generation, 4u);
  EXPECT_EQ(producer.rewindDepthSeconds(*order),
            std::optional<uint64_t>((block1.block.slot - harness_.genesis.block.slot) * producer.getSlotDuration()))
      << "slot time from the cutover block to the last block it drops";
}

// beacon-authority ba1: the recovery set and beacon certificate are not config — a config update
// must carry them unchanged (only their own records will change them).
TEST_F(ChainComposeTest, ConfigUpdate_CannotChangeTheAuthority) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  ConfigUpdateProbe probe(harness_);
  const std::string why = "cannot change the recovery set or beacon certificate";
  const std::vector<std::function<void(Chain::GenesisAccountMeta &)>> changes = {
      [](auto &gm) { gm.authority.recovery.delayEpochs += 1; },
      [](auto &gm) { gm.authority.recovery.publicKeys.push_back(makeKeyPair().publicKey); },
      [](auto &gm) { gm.authority.beacon.generation = 2; },
      [](auto &gm) { gm.authority.beacon.publicKey = makeKeyPair().publicKey; },
      [](auto &gm) { gm.authority = {}; },
  };
  for (size_t i = 0; i < changes.size(); ++i) {
    auto added = probe.addMeta(changes[i]);
    ASSERT_FALSE(added.isOk()) << "change " << i;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  }
  auto unchanged = probe.addMeta([](auto &gm) { gm.config.maxTransactionsPerBlock += 1; });
  EXPECT_TRUE(unchanged.isOk()) << unchanged.error().message;
}

// Liveness settings in a config update (WIRE_SCHEMA § Config updates): heartbeats at least once an
// epoch; a miner due period of 0 or at least 12 hours of slots (5 s slots: 8640).
TEST_F(ChainComposeTest, ConfigUpdate_RefusesLivenessSettingsOutOfRange) {
  auto &producer = harness_.producer;
  ASSERT_EQ(harness_.chainConfig.slotDuration, 5u);
  ASSERT_EQ(harness_.chainConfig.slotsPerEpoch, 10u);
  producer.setClockOverride(producer.getSlotStartTime(1));
  ConfigUpdateProbe probe(harness_);
  auto tryConfig = [&](const std::function<void(Chain::BlockChainConfig &)> &change) { return probe.add(change); };
  auto refused = tryConfig([](auto &c) { c.heartbeatSlots = 11; });
  ASSERT_FALSE(refused.isOk());
  EXPECT_NE(refused.error().message.find("heartbeatSlots (11) cannot exceed slotsPerEpoch (10)"), std::string::npos)
      << refused.error().message;
  refused = tryConfig([](auto &c) { c.minerDueAfterSlots = 8639; });
  ASSERT_FALSE(refused.isOk());
  EXPECT_NE(refused.error().message.find("at least 8640"), std::string::npos) << refused.error().message;
  for (const auto &ok : std::vector<std::function<void(Chain::BlockChainConfig &)>>{
           [](auto &c) { c.minerDueAfterSlots = 8640; }, [](auto &c) { c.minerDueAfterSlots = 0; },
           [](auto &c) { c.heartbeatSlots = 0; }, [](auto &c) { c.heartbeatSlots = 10; }}) {
    auto added = tryConfig(ok);
    EXPECT_TRUE(added.isOk()) << added.error().message;
  }
}

TEST_F(ChainComposeTest, GenesisIssuance_CappedPerEpoch) {
  auto &producer = harness_.producer;
  auto issue = [&](uint64_t amount, uint64_t idem) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = AccountBuffer::ID_GENESIS;
    tx.toWalletId = AccountBuffer::ID_RESERVE;
    tx.amount = amount;
    tx.fee = 1;
    tx.idempotentId = idem;
    tx.validationTsMin = harness_.chainConfig.genesisTime;
    tx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
    return makeRecord(Ledger::T_DEFAULT, tx, harness_.genesisKey);
  };
  const int64_t reserveBefore = producer.getAccount(AccountBuffer::ID_RESERVE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS);

  // Epoch 0: 60 + 1 fee issued; another 50 + 1 would pass 100.
  Ledger::ChainNode block1 = makeNextBlockAtSlot(producer, harness_.genesis, 1, {issue(60, 1)});
  ASSERT_TRUE(producer.addBlock(block1).isOk());
  {
    auto leader = producer.getSlotLeader(2);
    ASSERT_TRUE(leader.isOk());
    auto block = producer.linkNextBlock(block1, 2, leader.value(), producer.getSlotEndTime(2), {issue(50, 2)});
    auto sealed = producer.sealBlock(block);
    ASSERT_FALSE(sealed.isOk());
    EXPECT_NE(sealed.error().message.find("maxIssuancePerEpoch"), std::string::npos) << sealed.error().message;
    producer.abandonSeal();
  }

  // Epoch 1: a fresh allowance.
  Ledger::ChainNode block2 =
      makeNextBlockAtSlot(producer, block1, harness_.chainConfig.slotsPerEpoch, {issue(90, 3)});
  ASSERT_TRUE(producer.addBlock(block2).isOk());
  EXPECT_EQ(producer.getAccount(AccountBuffer::ID_RESERVE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS),
            reserveBefore + 150);
}

// Miner state is chain-owned: an account cannot be created already registered (only genesis
// miners are, in the genesis block), and an update cannot change it — otherwise anyone could
// register without the fee and minimum stake, or reset its own missed-slot count.
TEST_F(ChainComposeTest, MinerStateCannotBeSetByAccountCreationOrUpdate) {
  auto &producer = harness_.producer;
  EXPECT_TRUE(producer.getAccount(kTestMinerId).value().wallet.miner.registered) << "genesis miner";
  const int64_t now = static_cast<int64_t>(std::time(nullptr));  // buffer admission checks wall time
  auto leader = producer.getSlotLeader(1);
  ASSERT_TRUE(leader.isOk());

  auto userKey = makeKeyPair();
  Client::UserAccount selfRegistered = makeUserAccount(userKey.publicKey, 10);
  selfRegistered.wallet.miner.registered = true;
  Ledger::TxNewUser nu;
  nu.fromWalletId = AccountBuffer::ID_RESERVE;
  nu.toWalletId = AccountBuffer::ID_FIRST_USER + 7;
  nu.amount = 10;
  nu.fee = 100;
  nu.meta = selfRegistered.ltsToString();
  nu.idempotentId = 1;
  nu.validationTsMin = now - 60;
  nu.validationTsMax = now + 60;
  {
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_NEW_USER, nu, harness_.reserveKey, producer.getNetworkId()), leader.value());
    ASSERT_FALSE(added.isOk());
    EXPECT_NE(added.error().message.find("miner state"), std::string::npos) << added.error().message;
  }

  // The genesis miner tries to reset its own state through a self-update.
  Client::UserAccount update;
  update.wallet = producer.getAccount(kTestMinerId).value().wallet;
  update.wallet.miner.missedDue = 0;
  update.wallet.miner.lastProducedSlot = 999;
  update.wallet.mBalances[AccountBuffer::ID_GENESIS] -= 100;  // pays its fee
  update.meta = AccountAttachment::emptySerialized();
  Ledger::TxUserUpdate uu;
  uu.walletId = kTestMinerId;
  uu.fee = 100;
  uu.meta = update.ltsToString();
  uu.idempotentId = 2;
  uu.validationTsMin = now - 60;
  uu.validationTsMax = now + 60;
  {
    AccountBuffer scratch;
    auto added = producer.addBufferTransaction(
        scratch, makeRecord(Ledger::T_USER_UPDATE, uu, harness_.reserveKey, producer.getNetworkId()), leader.value());
    ASSERT_FALSE(added.isOk());
    EXPECT_NE(added.error().message.find("miner state"), std::string::npos) << added.error().message;
  }
}

namespace {

/** A funded user account (block at `slot`), for miner-registration tests. */
struct FundedUser {
  uint64_t id = 0;
  utl::MlDsaKeyPair key;
  Ledger::ChainNode block;
};

FundedUser addFundedUser(ComposeHarness &h, const Ledger::ChainNode &previous, uint64_t slot, uint64_t id,
                         int64_t balance) {
  FundedUser user;
  user.id = id;
  user.key = makeKeyPair();
  Ledger::TxNewUser nu;
  nu.fromWalletId = AccountBuffer::ID_RESERVE;
  nu.toWalletId = id;
  nu.amount = static_cast<uint64_t>(balance);
  nu.fee = 6;  // meta fee 1 + newAccountFee 5
  nu.meta = makeUserAccount(user.key.publicKey, balance).ltsToString();
  nu.idempotentId = id;
  nu.validationTsMin = h.chainConfig.genesisTime;
  nu.validationTsMax = h.chainConfig.genesisTime + 3600;
  user.block = makeNextBlockAtSlot(h.producer, previous, slot,
                                   {makeRecord(Ledger::T_NEW_USER, nu, h.reserveKey, h.producer.getNetworkId())});
  EXPECT_TRUE(h.producer.addBlock(user.block).isOk());
  return user;
}

Ledger::Record minerTx(ComposeHarness &h, const FundedUser &user, uint16_t type, uint64_t fee, uint64_t idem) {
  Ledger::TxMinerRegister tx;
  tx.walletId = user.id;
  tx.fee = fee;
  tx.idempotentId = idem;
  tx.validationTsMin = h.chainConfig.genesisTime;
  tx.validationTsMax = h.chainConfig.genesisTime + 3600;
  Ledger::Record rec;
  rec.type = type;
  rec.data = utl::binaryPack(static_cast<const Ledger::TxMiner &>(tx));
  rec.signatures = {signMessage(user.key, rec.signingMessage(h.producer.getNetworkId()))};
  return rec;
}

} // namespace

// Registration: the account opts in itself, pays the meta fee + minerRegistrationFee (7), keeps
// at least minerMinStake (50) after the fee; revoke opts out. System accounts never mine.
TEST_F(ChainComposeTest, MinerRegisterAndRevoke) {
  auto &producer = harness_.producer;
  FundedUser rich = addFundedUser(harness_, harness_.genesis, 1, AccountBuffer::ID_FIRST_USER + 1, 1000);
  FundedUser poor = addFundedUser(harness_, rich.block, 2, AccountBuffer::ID_FIRST_USER + 2, 55);
  producer.setClockOverride(producer.getSlotStartTime(3));  // the buffer path checks validity windows
  auto tryAdd = [&](const Ledger::Record &rec) {
    AccountBuffer scratch;
    return producer.addBufferTransaction(scratch, rec, producer.getSlotLeader(3).value());
  };
  auto refusedWith = [&](const Ledger::Record &rec, const std::string &why) {
    auto added = tryAdd(rec);
    ASSERT_FALSE(added.isOk()) << why;
    EXPECT_NE(added.error().message.find(why), std::string::npos) << added.error().message;
  };

  refusedWith(minerTx(harness_, rich, Ledger::T_MINER_REGISTER, 7, 10), "fee below minimum 8");
  refusedWith(minerTx(harness_, poor, Ledger::T_MINER_REGISTER, 8, 10), "minerMinStake");  // 55 - 8 < 50
  refusedWith(minerTx(harness_, rich, Ledger::T_MINER_REVOKE, 1, 11), "not a registered miner");
  FundedUser reserve{AccountBuffer::ID_RESERVE, harness_.reserveKey, {}};
  refusedWith(minerTx(harness_, reserve, Ledger::T_MINER_REGISTER, 8, 12), "System accounts");

  // Registered in a block: fee paid to the fee account, due clock starts at this slot.
  const int64_t feeBefore =
      producer.getAccount(AccountBuffer::ID_FEE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS);
  Ledger::ChainNode registered = makeNextBlockAtSlot(producer, poor.block, 3,
                                                     {minerTx(harness_, rich, Ledger::T_MINER_REGISTER, 8, 20)});
  ASSERT_TRUE(producer.addBlock(registered).isOk());
  auto account = producer.getAccount(rich.id).value();
  EXPECT_TRUE(account.wallet.miner.registered);
  EXPECT_EQ(account.wallet.miner.lastProducedSlot, 3u);
  EXPECT_EQ(account.wallet.mBalances.at(AccountBuffer::ID_GENESIS), 1000 - 8);
  EXPECT_EQ(producer.getAccount(AccountBuffer::ID_FEE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS),
            feeBefore + 8);
  refusedWith(minerTx(harness_, rich, Ledger::T_MINER_REGISTER, 8, 21), "already a registered miner");

  // Revoke: meta fee only.
  Ledger::ChainNode revoked = makeNextBlockAtSlot(producer, registered, 4,
                                                  {minerTx(harness_, rich, Ledger::T_MINER_REVOKE, 1, 22)});
  ASSERT_TRUE(producer.addBlock(revoked).isOk());
  EXPECT_FALSE(producer.getAccount(rich.id).value().wallet.miner.registered);
}

// Only registered miners are elected; a registration joins the committee at the next epoch's
// snapshot (its first block), a revoke leaves it the same way. Funded accounts that never
// registered are never candidates (before: every funded account was, miner or not).
TEST_F(ChainComposeTest, OnlyRegisteredMinersAreElectedFromTheNextEpoch) {
  auto &producer = harness_.producer;
  auto committee = [&]() {
    std::set<uint64_t> ids;
    for (const auto &s : producer.getStakeholders()) {
      ids.insert(s.id);
    }
    return ids;
  };
  FundedUser miner = addFundedUser(harness_, harness_.genesis, 1, AccountBuffer::ID_FIRST_USER + 1, 1000);
  FundedUser idle = addFundedUser(harness_, miner.block, 2, AccountBuffer::ID_FIRST_USER + 2, 5000);
  Ledger::ChainNode registered = makeNextBlockAtSlot(producer, idle.block, 3,
                                                     {minerTx(harness_, miner, Ledger::T_MINER_REGISTER, 8, 30)});
  ASSERT_TRUE(producer.addBlock(registered).isOk());
  EXPECT_EQ(committee(), std::set<uint64_t>{kTestMinerId}) << "epoch 0 snapshot predates the registration";

  // Epoch 1 (slots 10..19): its first block takes the snapshot with the new miner.
  Ledger::ChainNode epoch1 =
      makeNextBlockAtSlot(producer, registered, 10, {makeReserveTransfer(harness_, 5, 31)});
  ASSERT_TRUE(producer.addBlock(epoch1).isOk());
  EXPECT_EQ(committee(), (std::set<uint64_t>{kTestMinerId, miner.id}));
  EXPECT_FALSE(committee().contains(idle.id)) << "funded but never registered";

  Ledger::ChainNode revoked = makeNextBlockAtSlot(producer, epoch1, 11,
                                                  {minerTx(harness_, miner, Ledger::T_MINER_REVOKE, 1, 32)});
  ASSERT_TRUE(producer.addBlock(revoked).isOk());
  EXPECT_TRUE(committee().contains(miner.id)) << "still in epoch 1's committee";
  Ledger::ChainNode epoch2 = makeNextBlockAtSlot(producer, revoked, 20, {makeReserveTransfer(harness_, 5, 33)});
  ASSERT_TRUE(producer.addBlock(epoch2).isOk());
  EXPECT_EQ(committee(), std::set<uint64_t>{kTestMinerId});
}

// Miner liveness (WIRE_SCHEMA § Miner registration and liveness): due after 3 slots, 2 misses in
// a row deregister, 2 epochs of cool-down.
class MinerLivenessTest : public ::testing::Test {
protected:
  void SetUp() override {
    harness_.SetUp([](Chain::BlockChainConfig &c) {
      c.minerDueAfterSlots = 3;
      c.minerMaxMissedSlots = 2;
      c.minerReregisterCooldownEpochs = 2;
    });
  }
  void TearDown() override { harness_.TearDown(); }

  void lead(uint64_t slot, uint64_t id) {
    harness_.producer.forceSlotLeader(slot, id);
    harness_.peer.forceSlotLeader(slot, id);
  }
  /** Seal on the producer, then admit on the peer (the full apply path) — both banks must agree. */
  Ledger::ChainNode commit(const Ledger::ChainNode &previous, uint64_t slot,
                           const std::vector<Ledger::Record> &records) {
    harness_.producer.setClockOverride(harness_.producer.getSlotStartTime(slot));
    harness_.peer.setClockOverride(harness_.peer.getSlotStartTime(slot));
    Ledger::ChainNode block = makeNextBlockAtSlot(harness_.producer, previous, slot, records);
    EXPECT_TRUE(harness_.producer.addBlock(block).isOk()) << "producer commit at slot " << slot;
    auto peerAdd = harness_.peer.addBlock(block);
    EXPECT_TRUE(peerAdd.isOk()) << "peer admit at slot " << slot << ": "
                                << (peerAdd.isOk() ? "" : peerAdd.error().message);
    return block;
  }
  Client::MinerState minerOf(uint64_t id) { return harness_.peer.getAccount(id).value().wallet.miner; }

  ComposeHarness harness_;
};

// A due miner's empty block is admitted however short the tip lag; a miner that is not due still
// needs the heartbeat lag.
TEST_F(MinerLivenessTest, DueMinerEmptyBlockAccepted) {
  auto &producer = harness_.producer;
  ASSERT_EQ(producer.getHeartbeatSlots(), 10u);
  ASSERT_EQ(minerOf(kTestMinerId).lastProducedSlot, 0u);

  // Slot 2: lag 2, last block 2 slots old — neither rule admits an empty block.
  lead(2, kTestMinerId);
  producer.setClockOverride(producer.getSlotStartTime(2));
  Ledger::ChainNode early = producer.linkNextBlock(harness_.genesis, 2, kTestMinerId, producer.getSlotEndTime(2), {});
  auto seal = producer.sealBlock(early);
  ASSERT_FALSE(seal.isOk()) << "not due yet";
  EXPECT_NE(seal.error().message.find("Empty heartbeat premature"), std::string::npos) << seal.error().message;
  EXPECT_FALSE(producer.isMinerDue(kTestMinerId, 2));

  // Slot 3: due (3 slots since slot 0): the empty block is accepted and resets the clock.
  lead(3, kTestMinerId);
  EXPECT_TRUE(producer.isMinerDue(kTestMinerId, 3));
  Ledger::ChainNode heartbeat = commit(harness_.genesis, 3, {});
  ASSERT_TRUE(heartbeat.block.records.empty());
  EXPECT_EQ(minerOf(kTestMinerId).lastProducedSlot, 3u);
  EXPECT_FALSE(producer.isMinerDue(kTestMinerId, 4));
}

// A skipped slot counts a miss only for a due elected leader; producing resets the count; the
// limit deregisters and the cool-down then refuses re-registration until it has passed.
TEST_F(MinerLivenessTest, DueMissesDeregisterAndCoolDownBlocksReregistration) {
  auto &producer = harness_.producer;
  for (uint64_t slot = 1; slot < 40; ++slot) {
    lead(slot, kTestMinerId);
  }
  FundedUser user = addFundedUser(harness_, harness_.genesis, 1, AccountBuffer::ID_FIRST_USER + 1, 1000);
  harness_.peer.setClockOverride(harness_.peer.getSlotStartTime(1));
  ASSERT_TRUE(harness_.peer.addBlock(user.block).isOk());
  Ledger::ChainNode block = commit(user.block, 2, {minerTx(harness_, user, Ledger::T_MINER_REGISTER, 8, 40)});
  ASSERT_EQ(minerOf(user.id).lastProducedSlot, 2u);

  // Slots 3..5 skipped: the user leads 4 (not due: 2 slots) and 5 (due: 3 slots) → one miss.
  lead(4, user.id);
  lead(5, user.id);
  block = commit(block, 6, {makeReserveTransfer(harness_, 5, 41)});
  EXPECT_EQ(minerOf(user.id).missedDue, 1u);
  EXPECT_EQ(minerOf(kTestMinerId).missedDue, 0u) << "the producer led slot 3 but was not due";

  // The user shows up: the count resets and its clock restarts.
  lead(7, user.id);
  block = commit(block, 7, {makeReserveTransfer(harness_, 5, 42)});
  EXPECT_EQ(minerOf(user.id).missedDue, 0u);
  EXPECT_EQ(minerOf(user.id).lastProducedSlot, 7u);

  // Epoch 1: two due misses in a row (slots 13, 14) deregister it, recording the epoch.
  block = commit(block, 10, {makeReserveTransfer(harness_, 5, 43)});
  lead(13, user.id);
  lead(14, user.id);
  block = commit(block, 15, {makeReserveTransfer(harness_, 5, 44)});
  const Client::MinerState dropped = minerOf(user.id);
  EXPECT_FALSE(dropped.registered);
  EXPECT_EQ(dropped.missedDue, 0u);
  EXPECT_EQ(dropped.autoDeregisteredEpoch, 1u);

  // Cool-down: 2 epochs from epoch 1 — refused in epoch 2, allowed from epoch 3.
  producer.setClockOverride(producer.getSlotStartTime(21));
  AccountBuffer scratch;
  auto refused = producer.addBufferTransaction(scratch, minerTx(harness_, user, Ledger::T_MINER_REGISTER, 8, 45),
                                               kTestMinerId);
  ASSERT_FALSE(refused.isOk());
  EXPECT_NE(refused.error().message.find("cool-down until epoch 3"), std::string::npos) << refused.error().message;
  block = commit(block, 30, {minerTx(harness_, user, Ledger::T_MINER_REGISTER, 8, 46)});
  EXPECT_TRUE(minerOf(user.id).registered);
  EXPECT_EQ(minerOf(user.id).autoDeregisteredEpoch, Client::MinerState::kNever);
}

// A genesis renewal is what a node starting from its checkpoint reads (BLOCK_PIPELINE open item
// E): it carries the config in force for its block and, separately, a config update still
// pending — never the pending update as if it were in force.
class GenesisRenewalConfigTest : public ::testing::Test {
protected:
  void SetUp() override {
    harness_.SetUp([](Chain::BlockChainConfig &c) {
      c.checkpoint.minBlocks = 2;
      c.checkpoint.minAgeSeconds = 0;
    });
  }
  void TearDown() override { harness_.TearDown(); }

  Ledger::Record signedByLeader(Ledger::Record r) {
    r.signatures = {signMessage(harness_.reserveKey, r.signingMessage(harness_.producer.getNetworkId()))};
    return r;
  }
  static Chain::GenesisAccountMeta genesisMetaOf(const Ledger::Record &r) {
    Chain::GenesisAccountMeta gm;
    EXPECT_TRUE(gm.ltsFromString(utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().meta));
    return gm;
  }
  static bool isGenesisRenewal(const Ledger::Record &r) {
    return r.type == Ledger::T_RENEWAL &&
           utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().walletId == AccountBuffer::ID_GENESIS;
  }

  ComposeHarness harness_;
};

TEST_F(GenesisRenewalConfigTest, CarriesConfigInForceAndPendingUpdate) {
  auto &producer = harness_.producer;
  const uint64_t before = producer.getMaxTransactionsPerBlock();
  // The update rotates genesis to 3 keys (a genesis renewal needs a valid genesis wallet).
  const std::vector<utl::MlDsaKeyPair> rotated = {makeKeyPair(), makeKeyPair(), makeKeyPair()};
  Chain::GenesisAccountMeta update;
  update.config = harness_.chainConfig;
  update.config.maxTransactionsPerBlock = before + 1;
  update.genesis.wallet = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
  update.genesis.wallet.publicKeys = {rotated[0].publicKey, rotated[1].publicKey, rotated[2].publicKey};
  update.genesis.wallet.minSignatures = 2;
  update.genesis.meta = AccountAttachment::emptySerialized();
  update.authority = producer.getAuthority().value();
  Ledger::TxConfig configTx;
  configTx.meta = update.ltsToString();
  configTx.idempotentId = 1;
  configTx.validationTsMin = harness_.chainConfig.genesisTime;
  configTx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
  Ledger::ChainNode block = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeRecord(Ledger::T_CONFIG, configTx, harness_.genesisKey, producer.getNetworkId())});
  ASSERT_TRUE(producer.addBlock(block).isOk());

  // Renew whatever falls due, slot by slot, until genesis does (still epoch 0: update pending).
  std::optional<Ledger::Record> genesisRenewal;
  for (uint64_t slot = 2; slot < 10 && !genesisRenewal; ++slot) {
    auto due = producer.collectRenewals(slot);
    ASSERT_TRUE(due.isOk()) << due.error().message;
    std::vector<Ledger::Record> records;
    for (const auto &r : due.value()) {
      records.push_back(signedByLeader(r));
      if (isGenesisRenewal(r)) {
        genesisRenewal = records.back();
      }
    }
    if (genesisRenewal) {
      // A renewal claiming the pending update is in force (the T_CONFIG's meta, as copied from
      // the latest genesis record) is refused.
      auto tampered = utl::binaryUnpack<Ledger::TxRenewal>(genesisRenewal->data).value();
      Chain::GenesisAccountMeta claimed = genesisMetaOf(*genesisRenewal);
      claimed.config = update.config;
      claimed.hasPending = false;
      claimed.pending = {};
      tampered.meta = claimed.ltsToString();
      Ledger::Record bad = *genesisRenewal;
      bad.data = utl::binaryPack(tampered);
      std::vector<Ledger::Record> badRecords;
      for (const auto &r : records) {
        badRecords.push_back(isGenesisRenewal(r) ? signedByLeader(bad) : r);
      }
      Ledger::ChainNode badBlock =
          producer.linkNextBlock(block, slot, kTestMinerId, producer.getSlotEndTime(slot), badRecords);
      auto seal = producer.sealBlock(badBlock);
      ASSERT_FALSE(seal.isOk());
      EXPECT_NE(seal.error().message.find("config in force"), std::string::npos) << seal.error().message;

      // It carries the recovery set and beacon certificate in force; one that alters them is refused.
      EXPECT_TRUE(genesisMetaOf(*genesisRenewal).authority == producer.getAuthority().value());
      auto altered = utl::binaryUnpack<Ledger::TxRenewal>(genesisRenewal->data).value();
      Chain::GenesisAccountMeta alteredMeta = genesisMetaOf(*genesisRenewal);
      alteredMeta.authority.recovery.delayEpochs += 1;
      altered.meta = alteredMeta.ltsToString();
      Ledger::Record badAuthority = *genesisRenewal;
      badAuthority.data = utl::binaryPack(altered);
      std::vector<Ledger::Record> badAuthorityRecords;
      for (const auto &r : records) {
        badAuthorityRecords.push_back(isGenesisRenewal(r) ? signedByLeader(badAuthority) : r);
      }
      Ledger::ChainNode badAuthorityBlock =
          producer.linkNextBlock(block, slot, kTestMinerId, producer.getSlotEndTime(slot), badAuthorityRecords);
      auto authoritySeal = producer.sealBlock(badAuthorityBlock);
      ASSERT_FALSE(authoritySeal.isOk());
      EXPECT_NE(authoritySeal.error().message.find("recovery set and beacon certificate in force"), std::string::npos)
          << authoritySeal.error().message;

      // The slot leader signs renewals: it cannot slip its own keys in as the admin set (ba7).
      auto seized = utl::binaryUnpack<Ledger::TxRenewal>(genesisRenewal->data).value();
      Chain::GenesisAccountMeta seizedMeta = genesisMetaOf(*genesisRenewal);
      seizedMeta.genesis.wallet.publicKeys = {makeKeyPair().publicKey, makeKeyPair().publicKey,
                                              makeKeyPair().publicKey};
      seized.meta = seizedMeta.ltsToString();
      Ledger::Record badKeys = *genesisRenewal;
      badKeys.data = utl::binaryPack(seized);
      std::vector<Ledger::Record> badKeysRecords;
      for (const auto &r : records) {
        badKeysRecords.push_back(isGenesisRenewal(r) ? signedByLeader(badKeys) : r);
      }
      Ledger::ChainNode badKeysBlock =
          producer.linkNextBlock(block, slot, kTestMinerId, producer.getSlotEndTime(slot), badKeysRecords);
      auto keysSeal = producer.sealBlock(badKeysBlock);
      ASSERT_FALSE(keysSeal.isOk()) << "a leader-signed renewal replaced the admin set";
      EXPECT_NE(keysSeal.error().message.find("admin set in force"), std::string::npos) << keysSeal.error().message;
    }
    block = makeNextBlockAtSlot(producer, block, slot, records);
    ASSERT_TRUE(producer.addBlock(block).isOk()) << "slot " << slot;
  }
  ASSERT_TRUE(genesisRenewal.has_value()) << "genesis never fell due";

  const Chain::GenesisAccountMeta carried = genesisMetaOf(*genesisRenewal);
  EXPECT_EQ(carried.config.maxTransactionsPerBlock, before) << "the config in force";
  ASSERT_TRUE(carried.hasPending);
  EXPECT_EQ(carried.pending.activationEpoch, 1u);
  EXPECT_EQ(carried.pending.config.maxTransactionsPerBlock, before + 1);
}

// beacon-authority ba5b.2 — late join (BLOCK_PIPELINE): a node mounts at the producer's previous
// checkpoint and warms up on CheckpointReplay, trusting blocks blindly and without config until the
// genesis renewal installs it. Every account renews before the current checkpoint, so from there it
// validates Full blocks against the same state as a node from genesis — live and after a restart.
class CheckpointJoinTest : public ::testing::Test {
protected:
  void SetUp() override {
    harness_.SetUp([this](Chain::BlockChainConfig &c) {
      c.checkpoint.minBlocks = 3;
      c.checkpoint.minAgeSeconds = minAgeSeconds_;
    });
    joinerDir_ = pp::test::uniqueTempPath("pp-ledger-checkpoint-joiner");
    std::error_code ec;
    std::filesystem::remove_all(joinerDir_, ec);
  }
  void TearDown() override {
    joiner_.reset();
    harness_.TearDown();
    std::error_code ec;
    std::filesystem::remove_all(joinerDir_, ec);
  }

  /** The producer commits a block at `slot`: the renewals due (signed by the leader) and `records`. */
  Ledger::ChainNode produce(const Ledger::ChainNode &previous, uint64_t slot,
                            std::vector<Ledger::Record> records = {}) {
    auto &producer = harness_.producer;
    producer.setClockOverride(producer.getSlotEndTime(slot));
    auto due = producer.collectRenewals(slot);
    EXPECT_TRUE(due.isOk()) << due.error().message;
    std::vector<Ledger::Record> all;
    for (const auto &r : due.value()) {
      Ledger::Record signed_ = r;
      signed_.signatures = {signMessage(harness_.reserveKey, r.signingMessage(producer.getNetworkId()))};
      all.push_back(signed_);
    }
    all.insert(all.end(), records.begin(), records.end());
    Ledger::ChainNode block = makeNextBlockAtSlot(producer, previous, slot, all);
    auto added = producer.addBlock(block);
    EXPECT_TRUE(added.isOk()) << "producer, slot " << slot << ": " << added.error().message;
    return block;
  }

  /**
   * A funded user (slot 1), then a T_CONFIG rotating to a 3-key admin set (slot 2): a genesis
   * renewal needs one, as a real network's genesis has. Returns the tip.
   */
  Ledger::ChainNode startChain() {
    auto &producer = harness_.producer;
    producer.setClockOverride(producer.getSlotEndTime(1));
    user_ = addFundedUser(harness_, harness_.genesis, 1, AccountBuffer::ID_FIRST_USER, 500);
    Chain::GenesisAccountMeta gm;
    gm.config = harness_.chainConfig;
    if (configUpdate_) {
      configUpdate_(gm.config); // in force from the next epoch
    }
    gm.genesis.wallet = producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet;
    gm.genesis.wallet.publicKeys = {makeKeyPair().publicKey, makeKeyPair().publicKey, makeKeyPair().publicKey};
    gm.genesis.wallet.minSignatures = 2;
    gm.genesis.meta = AccountAttachment::emptySerialized();
    gm.authority = producer.getAuthority().value();
    Ledger::TxConfig config;
    config.meta = gm.ltsToString();
    config.idempotentId = 1;
    config.validationTsMin = harness_.chainConfig.genesisTime;
    config.validationTsMax = harness_.chainConfig.genesisTime + 3600;
    slot_ = 2;
    return produce(user_.block, slot_,
                   {makeRecord(Ledger::T_CONFIG, config, harness_.genesisKey, producer.getNetworkId())});
  }

  /** A record's validity window around `slot` (the helpers' windows end an hour after genesis). */
  template <typename Tx> void windowAt(Tx &tx, uint64_t slot) {
    const int64_t start = harness_.producer.getSlotStartTime(slot);
    tx.validationTsMin = start - 60;
    tx.validationTsMax = start + 3600;
  }

  /** Reserve → fee transfer valid at `slot`. */
  Ledger::Record transferAt(uint64_t slot) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = AccountBuffer::ID_RESERVE;
    tx.toWalletId = AccountBuffer::ID_FEE;
    tx.amount = 10 + slot % 7;
    tx.fee = 1;
    tx.idempotentId = idem_++;
    windowAt(tx, slot);
    return makeRecord(Ledger::T_DEFAULT, tx, harness_.reserveKey, harness_.producer.getNetworkId());
  }

  /**
   * The reserve funds a new user account `id` with `balance`, valid at `slot`. `profileBytes` > 0
   * gives it a self-published profile that large (its renewal then costs more than the free meta).
   */
  Ledger::Record newUserAt(uint64_t slot, uint64_t id, const utl::MlDsaKeyPair &key, int64_t balance,
                           size_t profileBytes = 0) {
    Ledger::TxNewUser nu;
    nu.fromWalletId = AccountBuffer::ID_RESERVE;
    nu.toWalletId = id;
    nu.amount = static_cast<uint64_t>(balance);
    nu.fee = profileBytes == 0 ? 6 : 6 + profileBytes; // meta fee + newAccountFee 5, generously
    auto account = makeUserAccount(key.publicKey, balance);
    if (profileBytes > 0) {
      AccountAttachment attachment;
      attachment.profiles[id].data = std::string(profileBytes, 'p');
      account.meta = attachment.ltsToString();
    }
    nu.meta = account.ltsToString();
    nu.idempotentId = idem_++;
    windowAt(nu, slot);
    return makeRecord(Ledger::T_NEW_USER, nu, harness_.reserveKey, harness_.producer.getNetworkId());
  }

  /** Account `id` registers itself as a miner, valid at `slot`. */
  Ledger::Record registerMinerAt(uint64_t slot, uint64_t id, const utl::MlDsaKeyPair &key) {
    Ledger::TxMinerRegister tx;
    tx.walletId = id;
    tx.fee = 8; // meta fee 1 + minerRegistrationFee 7
    tx.idempotentId = idem_++;
    windowAt(tx, slot);
    Ledger::Record rec;
    rec.type = Ledger::T_MINER_REGISTER;
    rec.data = utl::binaryPack(static_cast<const Ledger::TxMiner &>(tx));
    rec.signatures = {signMessage(key, rec.signingMessage(harness_.producer.getNetworkId()))};
    return rec;
  }

  /** The next block, one slot on, with a reserve transfer (empty blocks wait for heartbeats). */
  Ledger::ChainNode next(const Ledger::ChainNode &tip) {
    ++slot_;
    return produce(tip, slot_, {transferAt(slot_)});
  }

  /** True when the genesis miner (the producer's key) leads `slot`. */
  bool genesisMinerLeads(uint64_t slot) {
    auto &producer = harness_.producer;
    producer.setClockOverride(producer.getSlotEndTime(slot));
    producer.refreshStakeholders(slot);
    if (!producer.ensureEpochSeed(producer.getEpochFromSlot(slot)).isOk()) {
      return false;
    }
    auto leader = producer.getSlotLeader(slot);
    return leader && leader.value() == kTestMinerId;
  }

  /** The next block in a slot the genesis miner leads; slots other miners lead are skipped. */
  Ledger::ChainNode nextLedByGenesisMiner(const Ledger::ChainNode &tip, std::vector<Ledger::Record> records = {}) {
    do {
      ++slot_;
    } while (!genesisMinerLeads(slot_));
    records.push_back(transferAt(slot_));
    return produce(tip, slot_, records);
  }

  /** Leaders forced on the producer (test hook); every joiner gets the same. */
  std::vector<std::pair<uint64_t, uint64_t>> forced_;
  void forceLeader(uint64_t slot, uint64_t id) {
    harness_.producer.forceSlotLeader(slot, id);
    forced_.emplace_back(slot, id);
  }

  /** A joiner as Miner::init makes one: consensus without config (it comes from the chain). */
  std::unique_ptr<Chain> openJoiner(uint64_t startingBlockId, bool fresh) {
    auto joiner = std::make_unique<Chain>();
    consensus::SlotCommittee::Config consensusConfig;
    consensusConfig.timeOffset = 0;
    joiner->initConsensus(consensusConfig);
    for (const auto &[slot, id] : forced_) {
      joiner->forceSlotLeader(slot, id);
    }
    if (fresh) {
      Ledger::InitConfig ledger;
      ledger.workDir = joinerDir_.string();
      ledger.startingBlockId = startingBlockId;
      EXPECT_TRUE(joiner->initLedger(ledger).isOk());
    } else {
      EXPECT_TRUE(joiner->mountLedger(joinerDir_.string()).isOk());
    }
    auto loaded = joiner->loadFromLedger(startingBlockId);
    EXPECT_TRUE(loaded.isOk()) << loaded.error().message;
    return joiner;
  }

  /**
   * A fresh joiner starts at checkpoint `start`, takes every block to the producer's tip (Full from
   * the next checkpoint on) and ends with the producer's state for `accounts`.
   */
  void joinFrom(uint64_t start, const std::vector<uint64_t> &accounts) {
    joiner_.reset();
    std::error_code ec;
    std::filesystem::remove_all(joinerDir_, ec);
    joiner_ = openJoiner(start, /*fresh=*/true);
    feed(*joiner_, start, harness_.producer.getNextBlockId());
    ASSERT_FALSE(HasFailure()) << "joining at checkpoint " << start;
    for (uint64_t id : accounts) {
      auto ours = joiner_->getAccount(id);
      auto theirs = harness_.producer.getAccount(id);
      ASSERT_EQ(ours.isOk(), theirs.isOk()) << "account " << id << ", joining at " << start;
      if (theirs.isOk()) {
        EXPECT_TRUE(ours.value().wallet == theirs.value().wallet) << "account " << id << ", joining at " << start;
      }
    }
  }

  /** Every checkpoint the producer rotated to (oldest first). */
  std::vector<uint64_t> rotations_;

  /** produce(), recording checkpoint rotations. */
  Ledger::ChainNode produceTracked(const Ledger::ChainNode &previous, uint64_t slot,
                                   std::vector<Ledger::Record> records) {
    const uint64_t before = harness_.producer.getCheckpoint().currentId;
    auto block = produce(previous, slot, std::move(records));
    if (harness_.producer.getCheckpoint().currentId != before) {
      rotations_.push_back(harness_.producer.getCheckpoint().currentId);
    }
    return block;
  }

  /** The joiner takes the producer's blocks [from, to). */
  void feed(Chain &joiner, uint64_t from, uint64_t to) {
    for (uint64_t id = from; id < to; ++id) {
      auto block = harness_.producer.readBlock(id);
      ASSERT_TRUE(block.isOk());
      joiner.setClockOverride(harness_.producer.getSlotEndTime(block.value().block.slot));
      auto added = joiner.addBlock(block.value());
      ASSERT_TRUE(added.isOk()) << "joiner, block " << id << ": " << added.error().message;
    }
  }

  ComposeHarness harness_;
  std::filesystem::path joinerDir_;
  std::unique_ptr<Chain> joiner_;
  FundedUser user_;
  uint64_t slot_ = 0;
  uint64_t idem_ = 1;
  uint64_t minAgeSeconds_ = 0;
  /** Applied to the config the startChain T_CONFIG installs. */
  std::function<void(Chain::BlockChainConfig &)> configUpdate_;
};

// A checkpoint is where a node can start (beacon-authority ba5b): the first block of an epoch, at
// least checkpoint.minBlocks after the current one, opening with the genesis renewal. The newest
// two are kept (a joiner starts at the older and works from the newer).
TEST_F(CheckpointJoinTest, CheckpointsAreEpochStartsOpeningWithTheGenesisRenewal) {
  auto &producer = harness_.producer;
  const uint64_t slotsPerEpoch = harness_.chainConfig.slotsPerEpoch;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  std::vector<uint64_t> rotations;
  while (rotations.size() < 2) {
    const auto before = producer.getCheckpoint();
    tip = next(tip);
    ASSERT_FALSE(HasFailure()) << "slot " << slot_;
    ASSERT_LT(slot_, 200u);
    const auto after = producer.getCheckpoint();
    if (after.currentId == before.currentId) {
      continue;
    }
    rotations.push_back(after.currentId);
    EXPECT_EQ(after.currentId, tip.block.index);
    EXPECT_EQ(after.lastId, before.currentId) << "the older of the two kept";
    EXPECT_GE(tip.block.index, before.currentId + harness_.chainConfig.checkpoint.minBlocks);
    EXPECT_EQ(tip.block.slot % slotsPerEpoch, 0u) << "one block per slot here: the epoch's first slot";
    ASSERT_FALSE(tip.block.records.empty());
    EXPECT_TRUE(chain_block::isGenesisRenewal(tip.block.records.front()));
  }

  // At the next rotation point, a block without the genesis renewal is refused.
  while (true) {
    const uint64_t candidateSlot = slot_ + 1;
    if (candidateSlot % slotsPerEpoch == 0 &&
        producer.getNextBlockId() >= producer.getCheckpoint().currentId + harness_.chainConfig.checkpoint.minBlocks) {
      break;
    }
    tip = next(tip);
    ASSERT_FALSE(HasFailure());
  }
  ++slot_;
  producer.setClockOverride(producer.getSlotEndTime(slot_));
  auto due = producer.collectRenewals(slot_);
  ASSERT_TRUE(due.isOk());
  std::vector<Ledger::Record> withoutGenesis;
  for (const auto &r : due.value()) {
    if (!chain_block::isGenesisRenewal(r)) {
      Ledger::Record signed_ = r;
      signed_.signatures = {signMessage(harness_.reserveKey, r.signingMessage(producer.getNetworkId()))};
      withoutGenesis.push_back(signed_);
    }
  }
  withoutGenesis.push_back(makeReserveTransfer(harness_, 3, idem_++));
  Ledger::ChainNode bad =
      producer.linkNextBlock(tip, slot_, kTestMinerId, producer.getSlotEndTime(slot_), withoutGenesis);
  auto sealed = producer.sealBlock(bad);
  ASSERT_FALSE(sealed.isOk());
  EXPECT_NE(sealed.error().message.find("must open with the genesis renewal"), std::string::npos)
      << sealed.error().message;
}

TEST_F(CheckpointJoinTest, JoinerAtThePreviousCheckpointValidatesFullBlocksLikeTheProducer) {
  auto &producer = harness_.producer;
  // Grow past two checkpoint rotations.
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  while (producer.getCheckpoint().lastId == 0 || producer.getNextBlockId() < producer.getCheckpoint().currentId + 2) {
    tip = next(tip);
    ASSERT_FALSE(HasFailure()) << "producing slot " << slot_;
    ASSERT_LT(slot_, 200u) << "no second checkpoint rotation";
  }
  const auto checkpoint = producer.getCheckpoint();
  ASSERT_GT(checkpoint.lastId, 0u);
  const uint64_t start = checkpoint.lastId; // what STATUS serves a joining miner

  joiner_ = openJoiner(start, /*fresh=*/true);
  EXPECT_FALSE(joiner_->isChainConfigReady()) << "config comes from the warm-up blocks";
  feed(*joiner_, start, checkpoint.currentId);
  ASSERT_FALSE(HasFailure());
  auto warming = joiner_->sealReadiness(slot_ + 1);
  ASSERT_FALSE(warming.isOk());
  EXPECT_NE(warming.error().message.find("warming up"), std::string::npos) << warming.error().message;
  feed(*joiner_, checkpoint.currentId, producer.getNextBlockId());
  ASSERT_FALSE(HasFailure());
  // Validating Full now, but it seals only once its blocks span the transaction validity window:
  // before that, a transaction committed earlier could pass its idempotency check again.
  auto young = joiner_->sealReadiness(slot_ + 1);
  ASSERT_FALSE(young.isOk());
  EXPECT_NE(young.error().message.find("Cannot seal yet"), std::string::npos) << young.error().message;
  const uint64_t windowSlots = harness_.chainConfig.maxValidationTimespanSeconds / harness_.chainConfig.slotDuration;
  EXPECT_TRUE(joiner_->sealReadiness(producer.readBlock(start).value().block.slot + windowSlots + 1).isOk());
  EXPECT_TRUE(joiner_->isChainConfigReady());
  EXPECT_TRUE(joiner_->getAuthority() == producer.getAuthority()) << "authority from the genesis renewal";
  EXPECT_EQ(joiner_->getCheckpoint().currentId, checkpoint.currentId);

  // Past the current checkpoint the joiner checks everything, stateRoot included.
  for (int i = 0; i < 3; ++i) {
    tip = next(tip);
    feed(*joiner_, tip.block.index, tip.block.index + 1);
    ASSERT_FALSE(HasFailure());
  }
  for (uint64_t id : {AccountBuffer::ID_GENESIS, AccountBuffer::ID_RESERVE, AccountBuffer::ID_FEE, kTestMinerId,
                      user_.id}) {
    auto ours = joiner_->getAccount(id);
    auto theirs = producer.getAccount(id);
    ASSERT_TRUE(ours.isOk() && theirs.isOk()) << "account " << id;
    EXPECT_TRUE(ours.value().wallet == theirs.value().wallet) << "account " << id;
  }

  // A restart replays the warm-up from disk the same way.
  joiner_.reset();
  joiner_ = openJoiner(start, /*fresh=*/false);
  EXPECT_TRUE(joiner_->isChainConfigReady());
  tip = next(tip);
  feed(*joiner_, tip.block.index, tip.block.index + 1);
}

// beacon-authority ba5b.3: the first block of each epoch carries the committee its
// stakeSnapshotHash commits to (a warming node takes the epoch's leaders from it); no other block
// carries one. A tampered committee is refused, by a node in warm-up too.
TEST_F(CheckpointJoinTest, EpochOpeningBlocksCarryTheirCommittee) {
  auto &producer = harness_.producer;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  while (producer.getCheckpoint().lastId == 0) {
    tip = next(tip);
    ASSERT_FALSE(HasFailure());
    ASSERT_LT(slot_, 200u);
  }
  for (uint64_t id = 1; id < producer.getNextBlockId(); ++id) {
    const auto block = producer.readBlock(id).value().block;
    const auto previous = producer.readBlock(id - 1).value().block;
    if (previous.epoch < block.epoch) {
      ASSERT_FALSE(block.committee.empty()) << "block " << id << " opens epoch " << block.epoch;
      EXPECT_EQ(chain_block::calculateStakeSnapshotHash(chain_block::fromCommittee(block.committee)),
                block.stakeSnapshotHash);
    } else {
      EXPECT_TRUE(block.committee.empty()) << "block " << id;
    }
  }

  const uint64_t start = producer.getCheckpoint().lastId;
  auto opening = producer.readBlock(start).value();
  ASSERT_FALSE(opening.block.committee.empty());
  auto tampered = opening;
  tampered.block.committee.front().stake += 1; // not in the block hash: only stakeSnapshotHash guards it
  joiner_ = openJoiner(start, /*fresh=*/true);
  auto refused = joiner_->addBlock(tampered);
  ASSERT_FALSE(refused.isOk());
  EXPECT_NE(refused.error().message.find("committee does not match"), std::string::npos) << refused.error().message;

  ASSERT_TRUE(joiner_->addBlock(opening).isOk());
  auto inner = producer.readBlock(start + 1).value();
  ASSERT_TRUE(inner.block.committee.empty());
  inner.block.committee = opening.block.committee;
  auto stray = joiner_->addBlock(inner);
  ASSERT_FALSE(stray.isOk());
  EXPECT_NE(stray.error().message.find("does not open its epoch"), std::string::npos) << stray.error().message;
}

// Warm-up applies every effect on the accounts a joiner holds, whoever else a transaction touches:
// users created before the joiner's checkpoint keep paying each other (and fees) while the joiner
// does not hold them yet — fees still reach the fee account, credits still reach known recipients.
TEST_F(CheckpointJoinTest, WarmUpAppliesEveryEffectOnHeldAccounts) {
  auto &producer = harness_.producer;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  // Users are created one every few blocks, so their renewals (and the joiner's knowledge of them)
  // fall at different blocks from each other and from the system accounts'.
  std::vector<std::pair<uint64_t, utl::MlDsaKeyPair>> users;
  auto addUser = [&]() {
    const uint64_t id = AccountBuffer::ID_FIRST_USER + 10 + users.size();
    users.emplace_back(id, makeKeyPair());
    return newUserAt(slot_, id, users.back().second, 100000);
  };
  // Two users created together, out of step with the system accounts' renewals: while warming up,
  // the joiner holds the fee account before it holds either of them, and they pay each other.
  ++slot_;
  tip = produce(tip, slot_, {transferAt(slot_)});
  ++slot_;
  {
    std::vector<Ledger::Record> pair{addUser()};
    pair.push_back(addUser());
    tip = produce(tip, slot_, pair);
  }
  ASSERT_FALSE(HasFailure());
  // Accounts too poor to renew (a large profile makes renewal cost more than their 5) are ended at
  // their renewal (T_END_USER), their balances recycled — some before the joiner's checkpoint, some
  // during its warm-up while it does not hold them.
  std::vector<uint64_t> poor;
  auto addPoorUser = [&]() {
    const uint64_t id = AccountBuffer::ID_FIRST_USER + 100 + poor.size();
    poor.push_back(id);
    return newUserAt(slot_, id, makeKeyPair(), 5, /*profileBytes=*/700);
  };
  auto userTransfer = [&](int from, uint64_t to, uint64_t amount) {
    Ledger::TxDefault tx;
    tx.tokenId = AccountBuffer::ID_GENESIS;
    tx.fromWalletId = users[static_cast<size_t>(from)].first;
    tx.toWalletId = to;
    tx.amount = amount;
    tx.fee = 2;
    tx.idempotentId = idem_++;
    windowAt(tx, slot_);
    return makeRecord(Ledger::T_DEFAULT, tx, users[static_cast<size_t>(from)].second, producer.getNetworkId());
  };
  // Every block: one user pays the next user, another pays the reserve; plus the usual transfer;
  // every third block a new user (up to eight).
  auto traffic = [&]() {
    const size_t n = users.size();
    const int a = static_cast<int>(slot_ % n);
    const int b = static_cast<int>((slot_ + 1) % n);
    std::vector<Ledger::Record> records{userTransfer(0, users[1].first, 5), userTransfer(1, users[0].first, 4),
                                        userTransfer(a, users[static_cast<size_t>(b)].first, 3 + slot_ % 5),
                                        userTransfer(b, AccountBuffer::ID_RESERVE, 1), transferAt(slot_)};
    if (slot_ % 3 == 0 && users.size() < 8) {
      records.push_back(addUser());
    }
    if (slot_ % 4 == 1 && poor.size() < 6) {
      records.push_back(addPoorUser());
    }
    return records;
  };
  // Long enough for several checkpoints; a joiner starts from each (each warm-up meets the
  // traffic at a different point of the accounts' renewal cycles).
  while (rotations_.size() < 6) {
    ++slot_;
    tip = produceTracked(tip, slot_, traffic());
    ASSERT_FALSE(HasFailure()) << "slot " << slot_;
    ASSERT_LT(slot_, 400u);
  }
  std::vector<uint64_t> ids = {AccountBuffer::ID_GENESIS, AccountBuffer::ID_RESERVE, AccountBuffer::ID_FEE,
                               AccountBuffer::ID_RECYCLE, kTestMinerId};
  for (const auto &user : users) {
    ids.push_back(user.first);
  }
  ids.insert(ids.end(), poor.begin(), poor.end());
  size_t ended = 0;
  for (uint64_t id : poor) {
    ended += producer.getAccount(id).isOk() ? 0 : 1;
  }
  EXPECT_GT(ended, 0u) << "some poor accounts were ended";
  EXPECT_GT(producer.getAccount(AccountBuffer::ID_RECYCLE).value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 0)
      << "their balances were recycled";
  for (size_t k = 0; k + 1 < rotations_.size(); ++k) {
    joinFrom(rotations_[k], ids);
    ASSERT_FALSE(HasFailure());
  }
}

// Renewal due-sets and checkpoint rotation read block slot times, never a node's clock: a node
// replaying the chain from genesis years later (Full, every renewal checked) agrees with the
// producer. With checkpointMinAgeSeconds > 0 a clock-based cut-off would ask other accounts to
// renew and refuse history.
class CheckpointClockTest : public CheckpointJoinTest {
protected:
  void SetUp() override {
    minAgeSeconds_ = 30; // six 5 s slots
    CheckpointJoinTest::SetUp();
  }
};

TEST_F(CheckpointClockTest, ReplayYearsLaterAgreesWithTheProducer) {
  auto &producer = harness_.producer;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  while (producer.getCheckpoint().lastId == 0) {
    tip = next(tip);
    ASSERT_FALSE(HasFailure()) << "slot " << slot_;
    ASSERT_LT(slot_, 300u) << "no second checkpoint rotation";
  }

  Chain replay;
  consensus::SlotCommittee::Config consensusConfig;
  replay.initConsensus(consensusConfig);
  Ledger::InitConfig ledger;
  ledger.workDir = joinerDir_.string();
  ASSERT_TRUE(replay.initLedger(ledger).isOk());
  const int64_t tenYears = 10LL * 365 * 24 * 3600;
  for (uint64_t id = 0; id < producer.getNextBlockId(); ++id) {
    auto block = producer.readBlock(id);
    ASSERT_TRUE(block.isOk());
    replay.setClockOverride(harness_.chainConfig.genesisTime + tenYears);
    auto added = replay.addBlock(block.value());
    ASSERT_TRUE(added.isOk()) << "block " << id << ": " << added.error().message;
  }
  EXPECT_EQ(replay.getCheckpoint().lastId, producer.getCheckpoint().lastId);
  EXPECT_EQ(replay.getCheckpoint().currentId, producer.getCheckpoint().currentId);
}


// beacon-authority ba5b.4: warm-up is exact although liveness charges depend on each epoch's
// committee. Miner B (large stake, registered long before the joiner's checkpoint) never produces;
// once due, every slot it leads is skipped and charged — in the joiner's first epoch too, when the
// joiner holds only the genesis account and could not compute the committee itself. Miner C
// registers during the warm-up. From the current checkpoint the joiner validates Full blocks.
class CheckpointLivenessTest : public CheckpointJoinTest {
protected:
  void SetUp() override {
    CheckpointJoinTest::SetUp();
    configUpdate_ = [](Chain::BlockChainConfig &c) {
      c.minerDueAfterSlots = 8640; // the least a config allows at 5 s slots (12 hours)
      c.minerMaxMissedSlots = 0;   // keep counting misses (no auto-deregistration)
    };
  }

  Client::MinerState minerOf(const Chain &chain, uint64_t id) {
    auto account = chain.getAccount(id);
    EXPECT_TRUE(account.isOk()) << "account " << id;
    return account ? account.value().wallet.miner : Client::MinerState{};
  }

  static constexpr uint64_t kMinerB = AccountBuffer::ID_FIRST_USER + 1;
  static constexpr uint64_t kMinerC = AccountBuffer::ID_FIRST_USER + 2;
};

TEST_F(CheckpointLivenessTest, WarmUpChargesSkippedSlotsExactly) {
  auto &producer = harness_.producer;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  const auto keyB = makeKeyPair();
  tip = nextLedByGenesisMiner(tip, {newUserAt(slot_ + 1, kMinerB, keyB, 2000000)});
  ASSERT_FALSE(HasFailure());
  tip = nextLedByGenesisMiner(tip, {registerMinerAt(slot_ + 1, kMinerB, keyB)});
  ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(minerOf(producer, kMinerB).registered);

  // B becomes due 8640 slots after registering; jump past that and keep producing only in the
  // genesis miner's slots, so every slot B leads is skipped and charged. A new miner registers
  // after each checkpoint. Then a joiner starts from every checkpoint after the jump: each warm-up
  // meets the charges at a different point of the accounts' renewal cycles.
  slot_ += 8650;
  rotations_.clear();
  const uint64_t missedBefore = minerOf(producer, kMinerB).missedDue;
  std::vector<uint64_t> newMiners;
  size_t registeredFor = 0;
  while (rotations_.size() < 6) {
    std::vector<Ledger::Record> records;
    const auto key = makeKeyPair();
    const bool registerNow = rotations_.size() > registeredFor;
    const uint64_t id = kMinerC + newMiners.size();
    if (registerNow) {
      records.push_back(newUserAt(slot_ + 1, id, key, 900000));
    }
    do {
      ++slot_;
    } while (!genesisMinerLeads(slot_));
    records.push_back(transferAt(slot_));
    tip = produceTracked(tip, slot_, records);
    ASSERT_FALSE(HasFailure()) << "slot " << slot_;
    if (registerNow) {
      do {
        ++slot_;
      } while (!genesisMinerLeads(slot_));
      tip = produceTracked(tip, slot_, {registerMinerAt(slot_, id, key), transferAt(slot_)});
      ASSERT_FALSE(HasFailure()) << "slot " << slot_;
      newMiners.push_back(id);
      registeredFor = rotations_.size();
    }
    ASSERT_LT(slot_, 8650u + 20000u) << "too few checkpoints after the jump";
  }
  ASSERT_GT(minerOf(producer, kMinerB).missedDue, missedBefore) << "B charged after the jump";

  std::vector<uint64_t> ids = {kTestMinerId, kMinerB, AccountBuffer::ID_GENESIS, AccountBuffer::ID_RESERVE,
                               AccountBuffer::ID_FEE};
  ids.insert(ids.end(), newMiners.begin(), newMiners.end());
  for (size_t k = 0; k + 1 < rotations_.size(); ++k) {
    joinFrom(rotations_[k], ids);
    ASSERT_FALSE(HasFailure());
  }
}

// The checkpoint block's own liveness step, deterministically (leaders forced on the producer and
// every joiner): each epoch's first two slots are B's and skipped, so the epoch's first block — a
// checkpoint — charges B for them. Blocks per epoch are chosen so that every other checkpoint block
// also renews B: a joiner starting there holds B when that block's liveness runs (the block before
// it is not held; the charges start at the epoch's first slot all the same).
TEST_F(CheckpointLivenessTest, CheckpointBlockChargesExactly) {
  auto &producer = harness_.producer;
  const uint64_t slotsPerEpoch = harness_.chainConfig.slotsPerEpoch;
  const uint64_t minBlocks = harness_.chainConfig.checkpoint.minBlocks;
  Ledger::ChainNode tip = startChain();
  ASSERT_FALSE(HasFailure());
  auto produceLedByGenesisMiner = [&](uint64_t slot, std::vector<Ledger::Record> records) {
    forceLeader(slot, kTestMinerId);
    records.push_back(transferAt(slot));
    tip = produceTracked(tip, slot, records);
  };
  const auto keyB = makeKeyPair();
  ++slot_;
  produceLedByGenesisMiner(slot_, {newUserAt(slot_, kMinerB, keyB, 2000000)});
  ++slot_;
  produceLedByGenesisMiner(slot_, {registerMinerAt(slot_, kMinerB, keyB)});
  ASSERT_FALSE(HasFailure());
  // The block that last recorded B (its creation or a renewal): renewal falls due from it.
  auto lastRecordedB = [&]() -> uint64_t {
    for (uint64_t id = producer.getNextBlockId(); id-- > 0;) {
      const auto block = producer.readBlock(id);
      if (!block) {
        continue;
      }
      for (const auto &r : block.value().block.records) {
        if ((r.type == Ledger::T_RENEWAL && utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().walletId == kMinerB) ||
            (r.type == Ledger::T_NEW_USER && utl::binaryUnpack<Ledger::TxNewUser>(r.data).value().toWalletId == kMinerB)) {
          return id;
        }
      }
    }
    return 0;
  };

  slot_ += 8650; // B is due from here on
  rotations_.clear();
  std::vector<uint64_t> renewingB;
  const auto keyC = makeKeyPair();
  for (uint64_t round = 0; round < 8; ++round) {
    const uint64_t epoch = slot_ / slotsPerEpoch + 1;
    const uint64_t first = epoch * slotsPerEpoch;
    forceLeader(first, kMinerB);
    forceLeader(first + 1, kMinerB);
    // Pick how many blocks this epoch gets (at least minBlocks, so the next epoch's first block is
    // a checkpoint): renewals of B fall every minBlocks blocks; aim the next checkpoint at one of
    // them in even rounds, and away from them in odd rounds.
    const bool wantRenewal = round % 2 == 0;
    const uint64_t firstIndex = producer.getNextBlockId();
    uint64_t blocks = minBlocks;
    for (; blocks < minBlocks + 3; ++blocks) {
      uint64_t nextRenewal = std::max(lastRecordedB() + minBlocks, firstIndex);
      while (nextRenewal < firstIndex + blocks) {
        nextRenewal += minBlocks;
      }
      if ((nextRenewal == firstIndex + blocks) == wantRenewal) {
        break;
      }
    }
    for (uint64_t i = 0; i < blocks; ++i) {
      std::vector<Ledger::Record> records;
      if (round == 3 && i == 1) {
        records.push_back(newUserAt(first + 2 + i, kMinerC, keyC, 900000)); // a miner joins mid-window
      }
      if (round == 3 && i == 2) {
        records.push_back(registerMinerAt(first + 2 + i, kMinerC, keyC));
      }
      produceLedByGenesisMiner(first + 2 + i, records);
      ASSERT_FALSE(HasFailure()) << "round " << round << ", block " << i;
    }
    slot_ = first + 2 + blocks - 1;
    if (wantRenewal) {
      renewingB.push_back(firstIndex + blocks);
    }
  }
  // The next epoch's first block closes the last round.
  const uint64_t first = (slot_ / slotsPerEpoch + 1) * slotsPerEpoch;
  forceLeader(first, kMinerB);
  forceLeader(first + 1, kMinerB);
  produceLedByGenesisMiner(first + 2, {});
  ASSERT_FALSE(HasFailure());

  // Every even round's checkpoint block renewed B and charged it for the epoch's first two slots.
  for (uint64_t index : renewingB) {
    const auto block = producer.readBlock(index).value();
    ASSERT_TRUE(std::find(rotations_.begin(), rotations_.end(), index) != rotations_.end()) << "block " << index;
    bool renews = false;
    for (const auto &r : block.block.records) {
      renews = renews || (r.type == Ledger::T_RENEWAL &&
                          utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().walletId == kMinerB);
    }
    EXPECT_TRUE(renews) << "checkpoint " << index << " renews B";
  }
  ASSERT_TRUE(minerOf(producer, kMinerC).registered);

  const std::vector<uint64_t> ids = {kTestMinerId, kMinerB, kMinerC, AccountBuffer::ID_GENESIS,
                                     AccountBuffer::ID_RESERVE, AccountBuffer::ID_FEE};
  for (size_t k = 0; k + 1 < rotations_.size(); ++k) {
    joinFrom(rotations_[k], ids);
    ASSERT_FALSE(HasFailure());
  }
}

// A transfer to the payer itself used to mint: transferBalance read both balances before writing
// them, so the payee's write (+amount) overwrote the payer's (-amount). It is refused.
TEST_F(ChainComposeTest, TransferToSelfIsRefused) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  const auto reserveBefore = producer.getAccount(AccountBuffer::ID_RESERVE).value().wallet.mBalances;
  Ledger::TxDefault tx;
  tx.tokenId = AccountBuffer::ID_GENESIS;
  tx.fromWalletId = AccountBuffer::ID_RESERVE;
  tx.toWalletId = AccountBuffer::ID_RESERVE;
  tx.amount = 1000;
  tx.fee = 1;
  tx.idempotentId = 1;
  tx.validationTsMin = harness_.chainConfig.genesisTime;
  tx.validationTsMax = harness_.chainConfig.genesisTime + 3600;
  AccountBuffer scratch;
  auto added = producer.addBufferTransaction(
      scratch, makeRecord(Ledger::T_DEFAULT, tx, harness_.reserveKey, producer.getNetworkId()), kTestMinerId);
  ASSERT_FALSE(added.isOk()) << "a self-transfer must not apply";
  EXPECT_NE(added.error().message.find("itself"), std::string::npos) << added.error().message;
  EXPECT_EQ(producer.getAccount(AccountBuffer::ID_RESERVE).value().wallet.mBalances, reserveBefore);
}


// ba7: the first block of a recovery's epoch carries the genesis renewal built for that epoch — the
// new admin set, nothing pending — and the slot leader's draft takes it, though the committed chain
// still holds the recovery as pending (found by L-SMOKE-RECOVERY: leaders could not produce).
TEST_F(GenesisRenewalConfigTest, RenewalInTheRecoveryEpochCarriesTheNewAdminSet) {
  auto &producer = harness_.producer;
  const std::string net = producer.getNetworkId();
  const std::vector<utl::MlDsaKeyPair> admins = {makeKeyPair(), makeKeyPair(), makeKeyPair()};
  producer.setClockOverride(producer.getSlotStartTime(1));
  Ledger::ChainNode block = makeNextBlockAtSlot(
      producer, harness_.genesis, 1,
      {makeRecoveryRecord(admins, 2, testRecoveryKey(), net, 1, harness_.chainConfig.genesisTime)});
  ASSERT_TRUE(producer.addBlock(block).isOk());
  const uint64_t applyEpoch = producer.getAuthority()->pendingRecovery.applyEpoch;
  const uint64_t slot = applyEpoch * harness_.chainConfig.slotsPerEpoch;

  // The leader drafts once its slot has ended (its production slot is then this one).
  producer.setClockOverride(producer.getSlotEndTime(slot));
  auto due = producer.collectRenewals(slot);
  ASSERT_TRUE(due.isOk()) << due.error().message;
  std::vector<Ledger::Record> records;
  AccountBuffer draft;
  bool sawGenesis = false;
  for (const auto &r : due.value()) {
    records.push_back(signedByLeader(r));
    auto drafted = producer.addBufferTransaction(draft, records.back(), kTestMinerId);
    ASSERT_TRUE(drafted.isOk()) << "the leader's draft refused a renewal: " << drafted.error().message;
    if (isGenesisRenewal(r)) {
      sawGenesis = true;
      const auto gm = genesisMetaOf(r);
      EXPECT_FALSE(gm.authority.hasPendingRecovery);
      ASSERT_EQ(gm.genesis.wallet.publicKeys.size(), 3u);
      EXPECT_EQ(gm.genesis.wallet.publicKeys[0], admins[0].publicKey);
    }
  }
  ASSERT_TRUE(sawGenesis);
  block = makeNextBlockAtSlot(producer, block, slot, records);
  ASSERT_TRUE(producer.addBlock(block).isOk());
  EXPECT_FALSE(producer.getAuthority()->hasPendingRecovery);
  EXPECT_EQ(producer.getAccount(AccountBuffer::ID_GENESIS).value().wallet.publicKeys[0], admins[0].publicKey);
}

// The slot leader signs every renewal: it re-records an account as it is. A renewal that changes an
// account's keys (taking it over) or its meta is refused; only the fee leaves its balance.
TEST_F(GenesisRenewalConfigTest, RenewalCannotChangeAnAccountsKeysOrMeta) {
  auto &producer = harness_.producer;
  producer.setClockOverride(producer.getSlotStartTime(1));
  Ledger::ChainNode block =
      makeNextBlockAtSlot(producer, harness_.genesis, 1, {makeReserveTransfer(harness_, 5, 1)});
  ASSERT_TRUE(producer.addBlock(block).isOk());

  producer.setClockOverride(producer.getSlotEndTime(2));
  auto due = producer.collectRenewals(2);
  ASSERT_TRUE(due.isOk()) << due.error().message;
  std::optional<Ledger::Record> minerRenewal;
  for (const auto &r : due.value()) {
    if (r.type == Ledger::T_RENEWAL && utl::binaryUnpack<Ledger::TxRenewal>(r.data).value().walletId == kTestMinerId) {
      minerRenewal = r;
    }
  }
  ASSERT_TRUE(minerRenewal.has_value()) << "the genesis miner is due";
  auto tryRenewal = [&](const std::function<void(Client::UserAccount &)> &change) {
    auto tx = utl::binaryUnpack<Ledger::TxRenewal>(minerRenewal->data).value();
    Client::UserAccount account;
    EXPECT_TRUE(account.ltsFromString(tx.meta));
    change(account);
    tx.meta = account.ltsToString();
    Ledger::Record record = *minerRenewal;
    record.data = utl::binaryPack(tx);
    AccountBuffer draft;
    return producer.addBufferTransaction(draft, signedByLeader(record), kTestMinerId);
  };

  auto honest = tryRenewal([](Client::UserAccount &) {});
  EXPECT_TRUE(honest.isOk()) << honest.error().message;
  auto seized = tryRenewal([](Client::UserAccount &a) { a.wallet.publicKeys = {makeKeyPair().publicKey}; });
  ASSERT_FALSE(seized.isOk()) << "the leader took the account over";
  EXPECT_NE(seized.error().message.find("cannot change the keys"), std::string::npos) << seized.error().message;
  auto threshold = tryRenewal([](Client::UserAccount &a) { a.wallet.minSignatures += 1; });
  EXPECT_FALSE(threshold.isOk());
  auto meta = tryRenewal([](Client::UserAccount &a) {
    AccountAttachment attachment;
    AccountAttachment::ProfileSlot slot;
    slot.data = "planted";
    slot.seq = 1;
    attachment.profiles[AccountBuffer::ID_RESERVE] = slot;
    a.meta = attachment.ltsToString();
  });
  ASSERT_FALSE(meta.isOk());
  EXPECT_NE(meta.error().message.find("cannot change the meta"), std::string::npos) << meta.error().message;
}
