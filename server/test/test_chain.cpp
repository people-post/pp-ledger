#include "../../lib/BinaryPack.hpp"
#include "../../lib/Utilities.h"
#include "../AccountBuffer.h"
#include "../Chain.h"

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
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.meta = "test";
  return account;
}

std::string signTx(const utl::Ed25519KeyPair &keyPair,
                   const Ledger::Transaction &tx) {
  auto message = utl::binaryPack(tx);
  auto result = utl::ed25519Sign(keyPair.privateKey, message);
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
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
  gm.genesis.meta = "genesis";

  Ledger::ChainNode genesis;
  genesis.block.index = 0;
  genesis.block.timestamp = chainConfig.genesisTime;
  genesis.block.previousHash = "0";
  genesis.block.nonce = 0;
  genesis.block.slot = 0;
  genesis.block.slotLeader = 0;

  Ledger::SignedData<Ledger::Transaction> checkpointTx;
  checkpointTx.obj.type = Ledger::Transaction::T_GENESIS;
  checkpointTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.toWalletId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.amount = 0;
  checkpointTx.obj.fee = 0;
  checkpointTx.obj.meta = gm.ltsToString();
  checkpointTx.signatures.push_back(signTx(genesisKey, checkpointTx.obj));
  genesis.block.signedTxes.push_back(checkpointTx);

  Client::UserAccount feeAccount = makeUserAccount(feeKey.publicKey, 0);
  Ledger::SignedData<Ledger::Transaction> feeTx;
  feeTx.obj.type = Ledger::Transaction::T_NEW_USER;
  feeTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  feeTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  feeTx.obj.toWalletId = AccountBuffer::ID_FEE;
  feeTx.obj.amount = 0;
  const uint64_t feeNonFreeBytes =
      feeAccount.meta.size() > chainConfig.freeCustomMetaSize
          ? static_cast<uint64_t>(feeAccount.meta.size()) -
                chainConfig.freeCustomMetaSize
          : 0ULL;
  const int64_t feeWalletFee = static_cast<int64_t>(
      calculateMinimumFeeFromNonFreeMetaSize(chainConfig, feeNonFreeBytes));
  feeTx.obj.fee = feeWalletFee;
  feeTx.obj.meta = feeAccount.ltsToString();
  feeTx.signatures.push_back(signTx(genesisKey, feeTx.obj));
  genesis.block.signedTxes.push_back(feeTx);

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

  Ledger::SignedData<Ledger::Transaction> reserveTx;
  reserveTx.obj.type = Ledger::Transaction::T_NEW_USER;
  reserveTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  reserveTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  reserveTx.obj.toWalletId = AccountBuffer::ID_RESERVE;
  reserveTx.obj.amount = reserveAmount;
  reserveTx.obj.fee = reserveFee;
  reserveTx.obj.meta = reserveAccount.ltsToString();
  reserveTx.signatures.push_back(signTx(genesisKey, reserveTx.obj));
  genesis.block.signedTxes.push_back(reserveTx);

  Ledger::SignedData<Ledger::Transaction> recycleTx;
  recycleTx.obj.type = Ledger::Transaction::T_NEW_USER;
  recycleTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  recycleTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  recycleTx.obj.toWalletId = AccountBuffer::ID_RECYCLE;
  recycleTx.obj.amount = 0;
  recycleTx.obj.fee = recycleFee;
  recycleTx.obj.meta = recycleAccount.ltsToString();
  recycleTx.signatures.push_back(signTx(genesisKey, recycleTx.obj));
  genesis.block.signedTxes.push_back(recycleTx);

  genesis.hash = validator.calculateHash(genesis.block);
  return genesis;
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

  auto result = validator.addBlock(genesis, true);
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

  auto result = validator.addBlock(genesis, true);
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
  auto addResult = validator.addBlock(genesis, true);
  ASSERT_TRUE(addResult.isOk());

  uint64_t blockId = 0;
  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_GENESIS, blockId);
  ASSERT_TRUE(result.isOk());
  EXPECT_FALSE(result.value().empty());
  EXPECT_EQ(blockId, 0u);
  for (const auto &st : result.value()) {
    EXPECT_TRUE(st.obj.fromWalletId == AccountBuffer::ID_GENESIS ||
                st.obj.toWalletId == AccountBuffer::ID_GENESIS);
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
  auto addResult = validator.addBlock(genesis, true);
  ASSERT_TRUE(addResult.isOk());

  uint64_t blockId = validator.getNextBlockId();
  ASSERT_GT(blockId, 0u);

  auto result =
      validator.findTransactionsByWalletId(AccountBuffer::ID_GENESIS, blockId);
  ASSERT_TRUE(result.isOk());
  const auto &txes = result.value();
  EXPECT_FALSE(txes.empty());
  for (const auto &st : txes) {
    EXPECT_TRUE(st.obj.fromWalletId == AccountBuffer::ID_GENESIS ||
                st.obj.toWalletId == AccountBuffer::ID_GENESIS);
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
  auto addResult = validator.addBlock(genesis, true);
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
  auto addResult = validator.addBlock(genesis, true);
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
  auto addResult = validator.addBlock(genesis, true);
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
  auto addResult = validator.addBlock(genesis, true);
  ASSERT_TRUE(addResult.isOk());

  // Genesis block has 4 transactions at indices 0..3.
  for (size_t i = 0; i < genesis.block.signedTxes.size(); ++i) {
    auto result = validator.findTransactionByIndex(static_cast<uint64_t>(i));
    ASSERT_TRUE(result.isOk()) << "findTransactionByIndex(" << i << ") failed";
    EXPECT_EQ(result.value().obj.type, genesis.block.signedTxes[i].obj.type);
    EXPECT_EQ(result.value().obj.fromWalletId,
              genesis.block.signedTxes[i].obj.fromWalletId);
    EXPECT_EQ(result.value().obj.toWalletId,
              genesis.block.signedTxes[i].obj.toWalletId);
  }

  std::filesystem::remove_all(tempDir, ec);
}
