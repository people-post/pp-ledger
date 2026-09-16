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
  Ledger::ChainNode block;
  block.block.index = previous.block.index + 1;
  block.block.previousHash = previous.hash;
  block.block.slot = slot;
  block.block.timestamp = validator.getSlotStartTime(block.block.slot);
  const uint64_t epoch = validator.getEpochFromSlot(block.block.slot);
  auto seedRoe = validator.ensureEpochSeed(epoch);
  EXPECT_TRUE(seedRoe.isOk()) << (seedRoe.isOk() ? "" : seedRoe.error().message);
  auto leaderResult = validator.getSlotLeader(block.block.slot);
  EXPECT_TRUE(leaderResult.isOk())
      << (leaderResult.isOk() ? "" : leaderResult.error().message);
  block.block.slotLeader = leaderResult.isOk() ? leaderResult.value() : 0;
  block.block.txIndex =
      previous.block.txIndex + previous.block.records.size();
  block.block.records = records;
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
// by producer (seal path) and by a peer (validateNormalBlock path).
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

