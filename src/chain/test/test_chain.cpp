#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"
#include "AccountBuffer.h"
#include "Chain.h"
#include "BlockValidation.h"
#include "client/AccountAttachment.h"
#include "client/Client.h"

#include <gtest/gtest.h>

#include <filesystem>
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

Ledger::ChainNode makeGenesisBlock(Chain &validator,
                                   const Chain::BlockChainConfig &chainConfig,
                                   const utl::MlDsaKeyPair &genesisKey,
                                   const utl::MlDsaKeyPair &feeKey,
                                   const utl::MlDsaKeyPair &reserveKey,
                                   const utl::MlDsaKeyPair &recycleKey) {
  Chain::GenesisAccountMeta gm;
  gm.config = chainConfig;
  gm.genesis.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  gm.genesis.wallet.publicKeys = {genesisKey.publicKey};
  gm.genesis.wallet.minSignatures = 1;
  gm.genesis.wallet.keyType = Crypto::TK_ML_DSA_65;
  gm.genesis.meta = AccountAttachment::emptySerialized();

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

  Client::UserAccount feeAccount = makeUserAccount(feeKey.publicKey, 0);
  Ledger::TxNewUser feeTx;
  feeTx.fromWalletId = AccountBuffer::ID_GENESIS;
  feeTx.toWalletId = AccountBuffer::ID_FEE;
  feeTx.amount = 0;
  const uint64_t feeNonFreeBytes =
      feeAccount.meta.size() > chainConfig.freeCustomMetaSize
          ? static_cast<uint64_t>(feeAccount.meta.size()) -
                chainConfig.freeCustomMetaSize
          : 0ULL;
  const int64_t feeWalletFee = static_cast<int64_t>(
      calculateMinimumFeeFromNonFreeMetaSize(chainConfig, feeNonFreeBytes));
  feeTx.fee = static_cast<uint64_t>(feeWalletFee);
  feeTx.meta = feeAccount.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_NEW_USER, feeTx, genesisKey, chainConfig.networkId));

  Client::UserAccount reserveAccount = makeUserAccount(reserveKey.publicKey, 0);
  Client::UserAccount recycleAccount = makeUserAccount(recycleKey.publicKey, 0);

  const uint64_t recycleNonFreeBytes =
      recycleAccount.meta.size() > chainConfig.freeCustomMetaSize
          ? static_cast<uint64_t>(recycleAccount.meta.size()) -
                chainConfig.freeCustomMetaSize
          : 0ULL;
  const int64_t recycleFee = static_cast<int64_t>(
      calculateMinimumFeeFromNonFreeMetaSize(chainConfig, recycleNonFreeBytes));

  int64_t reserveAmount =
      static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY);
  int64_t reserveFee = 0;
  for (int i = 0; i < 2; ++i) {
    reserveAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = reserveAmount;
    const uint64_t reserveNonFreeBytes =
        reserveAccount.meta.size() > chainConfig.freeCustomMetaSize
            ? static_cast<uint64_t>(reserveAccount.meta.size()) -
                  chainConfig.freeCustomMetaSize
            : 0ULL;
    reserveFee = static_cast<int64_t>(calculateMinimumFeeFromNonFreeMetaSize(
        chainConfig, reserveNonFreeBytes));
    reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY) -
                    feeWalletFee - reserveFee - recycleFee;
  }
  reserveAccount.wallet.mBalances[AccountBuffer::ID_GENESIS] = reserveAmount;

  Ledger::TxNewUser reserveTx;
  reserveTx.fromWalletId = AccountBuffer::ID_GENESIS;
  reserveTx.toWalletId = AccountBuffer::ID_RESERVE;
  reserveTx.amount = static_cast<uint64_t>(reserveAmount);
  reserveTx.fee = static_cast<uint64_t>(reserveFee);
  reserveTx.meta = reserveAccount.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_NEW_USER, reserveTx, genesisKey,
                 chainConfig.networkId));

  Ledger::TxNewUser recycleTx;
  recycleTx.fromWalletId = AccountBuffer::ID_GENESIS;
  recycleTx.toWalletId = AccountBuffer::ID_RECYCLE;
  recycleTx.amount = 0;
  recycleTx.fee = static_cast<uint64_t>(recycleFee);
  recycleTx.meta = recycleAccount.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_NEW_USER, recycleTx, genesisKey,
                 chainConfig.networkId));

  auto sealResult = validator.sealBlock(genesis);
  EXPECT_TRUE(sealResult.isOk());
  return genesis;
}

Ledger::ChainNode makeNextBlockAtSlot(
    Chain &validator, const Ledger::ChainNode &previous, uint64_t slot,
    const std::vector<Ledger::Record> &records) {
  const uint64_t epoch = validator.getEpochFromSlot(slot);
  auto seedRoe = validator.ensureEpochSeed(epoch);
  EXPECT_TRUE(seedRoe.isOk()) << (seedRoe.isOk() ? "" : seedRoe.error().message);
  auto leaderResult = validator.getSlotLeader(slot);
  EXPECT_TRUE(leaderResult.isOk())
      << (leaderResult.isOk() ? "" : leaderResult.error().message);
  auto block = validator.linkNextBlock(
      previous, slot, leaderResult.isOk() ? leaderResult.value() : 0,
      validator.getSlotStartTime(slot), records);
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-findtx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-findtx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-findtx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-findtx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-findtx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-gettx";
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-gettx";
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

  // Genesis has 4 transactions (indices 0..3). Index 4 is out of range.
  auto result = validator.findTransactionByIndex(4);
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-gettx";
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

TEST(ChainTest, Checkpoint_RotateAndKeepRecentTwo) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();

  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);
  chainConfig.checkpoint.minBlocks = 1;
  chainConfig.checkpoint.minAgeSeconds = 0;
  // Consecutive empty seals advance one slot each; allow lag of 1.
  chainConfig.heartbeatSlots = 1;

  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir =
      std::filesystem::temp_directory_path() /
      "pp-ledger-chain-test-checkpoints";
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
  auto addGenesisResult = validator.addBlock(genesis);
  ASSERT_TRUE(addGenesisResult.isOk());
  auto checkpoint = validator.getCheckpoint();
  EXPECT_EQ(checkpoint.lastId, 0u);
  EXPECT_EQ(checkpoint.currentId, 0u);

  validator.refreshStakeholders();
  Ledger::ChainNode block1 = makeNextBlock(validator, genesis, {});
  auto addBlock1Result = validator.addBlock(block1);
  ASSERT_TRUE(addBlock1Result.isOk());
  checkpoint = validator.getCheckpoint();
  // With additional slotsPerEpoch spacing, checkpoint does not rotate yet.
  EXPECT_EQ(checkpoint.lastId, 0u);
  EXPECT_EQ(checkpoint.currentId, 0u);

  validator.refreshStakeholders();
  Ledger::ChainNode block2 = makeNextBlock(validator, block1, {});
  auto addBlock2Result = validator.addBlock(block2);
  ASSERT_TRUE(addBlock2Result.isOk());
  checkpoint = validator.getCheckpoint();
  // Still below requiredBlocks (minBlocks + slotsPerEpoch), no rotation yet.
  EXPECT_EQ(checkpoint.lastId, 0u);
  EXPECT_EQ(checkpoint.currentId, 0u);

  // Now add enough blocks to satisfy the new spacing requirement
  // requiredBlocks = checkpoint.currentId + minBlocks + slotsPerEpoch.
  const uint64_t firstCheckpointIndex =
      chainConfig.checkpoint.minBlocks + consensusConfig.slotsPerEpoch;

  Ledger::ChainNode prev = block2;
  for (uint64_t idx = 3; idx <= firstCheckpointIndex; ++idx) {
    validator.refreshStakeholders();
    Ledger::ChainNode next = makeNextBlock(validator, prev, {});
    auto addResult = validator.addBlock(next);
    ASSERT_TRUE(addResult.isOk());
    prev = next;
  }

  checkpoint = validator.getCheckpoint();
  EXPECT_EQ(checkpoint.lastId, 0u);
  EXPECT_EQ(checkpoint.currentId, firstCheckpointIndex);

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
      std::filesystem::temp_directory_path() /
      "pp-ledger-chain-test-zero-idempotent-id";
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
      genesis, slot, leaderResult.value(), validator.getSlotStartTime(slot),
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

  std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                  "pp-ledger-chain-test-nonincreasing-slot";
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
      validator.getSlotStartTime(badSlot), {rec2});
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

  std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                  "pp-ledger-chain-test-far-future-slot";
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
      validator.getSlotStartTime(farFutureSlot), {});
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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-arrival";
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

  std::filesystem::path tempDir = std::filesystem::temp_directory_path() /
                                  "pp-ledger-chain-test-reload-future";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  ASSERT_TRUE(validator.initLedger(ledgerConfig).isOk());

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
      std::filesystem::temp_directory_path() / "pp-ledger-chain-test-renewal";
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
      {AccountBuffer::ID_RECYCLE, &recycleKey}};
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
      std::filesystem::temp_directory_path() /
      "pp-ledger-chain-test-replay-old-index";
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
      std::filesystem::temp_directory_path() /
      "pp-ledger-chain-test-idempotency-replay";
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
      std::filesystem::temp_directory_path() /
      "pp-ledger-chain-test-late-joiner";
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

  void SetUp() {
    chainConfig = makeChainConfig(/*genesisTime=*/0);
    chainConfig.slotDuration = 5;
    chainConfig.slotsPerEpoch = 10;
    chainConfig.checkpoint.minBlocks = 100;
    chainConfig.checkpoint.minAgeSeconds = 0;

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

    producerDir = std::filesystem::temp_directory_path() /
                  "pp-ledger-compose-producer";
    peerDir = std::filesystem::temp_directory_path() / "pp-ledger-compose-peer";
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
  bad.block.timestamp = producer.getSlotStartTime(slot);
  bad.block.slotLeader = wrong;
  bad.block.epoch = slot / 10; // slotsPerEpoch from harness config
  bad.block.txIndex =
      harness_.genesis.block.txIndex + harness_.genesis.block.records.size();
  bad.block.records = {};
  bad.block.txRoot = chain_block::calculateTxRoot(bad.block.records);
  bad.block.stakeSnapshotHash =
      chain_block::calculateStakeSnapshotHash(producer.getStakeholders());
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
                                    producer.getSlotStartTime(tipSlot),
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
    forged.block.timestamp = peer.getSlotStartTime(slot);
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
  early.block.timestamp = producer.getSlotStartTime(earlySlot);
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

