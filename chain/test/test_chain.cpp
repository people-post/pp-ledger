#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"
#include "AccountBuffer.h"
#include "Chain.h"

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
  return cfg;
}

utl::Ed25519KeyPair makeKeyPair() {
  auto result = utl::ed25519Generate();
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
  account.wallet.keyType = Crypto::TK_ED25519;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.meta = "test";
  return account;
}

std::string signMessage(const utl::Ed25519KeyPair &keyPair,
                        const std::string &message) {
  auto result = utl::ed25519Sign(keyPair.privateKey, message);
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

template <typename TxT>
Ledger::Record makeRecord(uint16_t type, const TxT &tx,
                          const utl::Ed25519KeyPair &signer) {
  Ledger::Record rec;
  rec.type = type;
  rec.data = utl::binaryPack(tx);
  rec.signatures = {signMessage(signer, rec.data)};
  return rec;
}

Ledger::ChainNode makeGenesisBlock(Chain &validator,
                                   const Chain::BlockChainConfig &chainConfig,
                                   const utl::Ed25519KeyPair &genesisKey,
                                   const utl::Ed25519KeyPair &feeKey,
                                   const utl::Ed25519KeyPair &reserveKey,
                                   const utl::Ed25519KeyPair &recycleKey) {
  Chain::GenesisAccountMeta gm;
  gm.config = chainConfig;
  gm.genesis.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  gm.genesis.wallet.publicKeys = {genesisKey.publicKey};
  gm.genesis.wallet.minSignatures = 1;
  gm.genesis.wallet.keyType = Crypto::TK_ED25519;
  gm.genesis.meta = "genesis";

  Ledger::ChainNode genesis;
  genesis.block.index = 0;
  genesis.block.timestamp = chainConfig.genesisTime;
  genesis.block.previousHash = "0";
  genesis.block.nonce = 0;
  genesis.block.slot = 0;
  genesis.block.slotLeader = 0;

  Ledger::TxGenesis checkpointTx;
  checkpointTx.fee = 0;
  checkpointTx.meta = gm.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_GENESIS, checkpointTx, genesisKey));

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
  genesis.block.records.push_back(makeRecord(Ledger::T_NEW_USER, feeTx, genesisKey));

  Client::UserAccount reserveAccount = makeUserAccount(reserveKey.publicKey, 0);
  Client::UserAccount recycleAccount = makeUserAccount(recycleKey.publicKey, 0);
  recycleAccount.meta = "Account for recycling write-off balances";

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
      makeRecord(Ledger::T_NEW_USER, reserveTx, genesisKey));

  Ledger::TxNewUser recycleTx;
  recycleTx.fromWalletId = AccountBuffer::ID_GENESIS;
  recycleTx.toWalletId = AccountBuffer::ID_RECYCLE;
  recycleTx.amount = 0;
  recycleTx.fee = static_cast<uint64_t>(recycleFee);
  recycleTx.meta = recycleAccount.ltsToString();
  genesis.block.records.push_back(
      makeRecord(Ledger::T_NEW_USER, recycleTx, genesisKey));

  genesis.hash = validator.calculateHash(genesis.block);
  return genesis;
}

Ledger::ChainNode makeNextBlock(
    Chain &validator, const Ledger::ChainNode &previous,
    const std::vector<Ledger::Record> &records) {
  Ledger::ChainNode block;
  block.block.index = previous.block.index + 1;
  block.block.previousHash = previous.hash;
  block.block.nonce = 0;
  block.block.slot = previous.block.slot + 1;
  block.block.timestamp = validator.getSlotStartTime(block.block.slot);
  auto leaderResult = validator.getSlotLeader(block.block.slot);
  EXPECT_TRUE(leaderResult.isOk());
  block.block.slotLeader = leaderResult.isOk() ? leaderResult.value() : 0;
  block.block.txIndex =
      previous.block.txIndex + previous.block.records.size();
  block.block.records = records;
  block.hash = validator.calculateHash(block.block);
  return block;
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
  block.previousHash = "prev";
  block.nonce = 7;
  block.slot = 2;
  block.slotLeader = 3;

  std::string hash1 = validator.calculateHash(block);
  std::string hash2 = validator.calculateHash(block);
  EXPECT_EQ(hash1, hash2);

  block.nonce = 8;
  std::string hash3 = validator.calculateHash(block);
  EXPECT_NE(hash1, hash3);
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
  genesis.hash = "bad-hash";

  auto result = validator.addBlock(genesis);
  EXPECT_TRUE(result.isError());
  EXPECT_NE(result.error().message.find("Genesis block hash validation failed"),
            std::string::npos);
}

TEST(ChainTest, AddBlock_AddsValidGenesisBlock) {
  Chain validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  auto recycleKey = makeKeyPair();
  Chain::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig;
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

  consensus::Ouroboros::Config consensusConfig2;
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
  consensus::Ouroboros::Config consensusConfig;
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
