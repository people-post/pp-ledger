#include "../Validator.h"
#include "../AccountBuffer.h"
#include "../../lib/Utilities.h"
#include "../../lib/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace pp;

namespace {

Validator::BlockChainConfig makeChainConfig(int64_t genesisTime) {
  Validator::BlockChainConfig cfg;
  cfg.genesisTime = genesisTime;
  cfg.slotDuration = 5;
  cfg.slotsPerEpoch = 10;
  cfg.maxPendingTransactions = 1000;
  cfg.maxTransactionsPerBlock = 100;
  cfg.minFeePerTransaction = 0;
  cfg.checkpoint.minBlocks = 10;
  cfg.checkpoint.minAgeSeconds = 20;
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

Client::UserAccount makeUserAccount(const std::string &publicKey, int64_t balance) {
  Client::UserAccount account;
  account.wallet.publicKeys = {publicKey};
  account.wallet.minSignatures = 1;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.meta = "test";
  return account;
}

std::string signTx(const utl::Ed25519KeyPair &keyPair, const Ledger::Transaction &tx) {
  auto message = utl::binaryPack(tx);
  auto result = utl::ed25519Sign(keyPair.privateKey, message);
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

Ledger::ChainNode makeGenesisBlock(Validator &validator,
                                  const Validator::BlockChainConfig &chainConfig,
                                  const utl::Ed25519KeyPair &genesisKey,
                                  const utl::Ed25519KeyPair &feeKey,
                                  const utl::Ed25519KeyPair &reserveKey) {
  Validator::SystemCheckpoint checkpoint;
  checkpoint.config = chainConfig;
  checkpoint.genesis.wallet.mBalances[AccountBuffer::ID_GENESIS] = 0;
  checkpoint.genesis.wallet.publicKeys = {genesisKey.publicKey};
  checkpoint.genesis.wallet.minSignatures = 1;
  checkpoint.genesis.meta = "genesis";

  Ledger::ChainNode genesis;
  genesis.block.index = 0;
  genesis.block.timestamp = chainConfig.genesisTime;
  genesis.block.previousHash = "0";
  genesis.block.nonce = 0;
  genesis.block.slot = 0;
  genesis.block.slotLeader = 0;

  Ledger::SignedData<Ledger::Transaction> checkpointTx;
  checkpointTx.obj.type = Ledger::Transaction::T_CHECKPOINT;
  checkpointTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.toWalletId = AccountBuffer::ID_GENESIS;
  checkpointTx.obj.amount = 0;
  checkpointTx.obj.fee = 0;
  checkpointTx.obj.meta = checkpoint.ltsToString();
  checkpointTx.signatures.push_back(signTx(genesisKey, checkpointTx.obj));
  genesis.block.signedTxes.push_back(checkpointTx);

  Client::UserAccount feeAccount = makeUserAccount(feeKey.publicKey, 0);
  Ledger::SignedData<Ledger::Transaction> feeTx;
  feeTx.obj.type = Ledger::Transaction::T_NEW_USER;
  feeTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  feeTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  feeTx.obj.toWalletId = AccountBuffer::ID_FEE;
  feeTx.obj.amount = 0;
  feeTx.obj.fee = 0;
  feeTx.obj.meta = feeAccount.ltsToString();
  feeTx.signatures.push_back(signTx(genesisKey, feeTx.obj));
  genesis.block.signedTxes.push_back(feeTx);

  int64_t reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY - chainConfig.minFeePerTransaction);
  Client::UserAccount reserveAccount = makeUserAccount(reserveKey.publicKey, reserveAmount);
  Ledger::SignedData<Ledger::Transaction> reserveTx;
  reserveTx.obj.type = Ledger::Transaction::T_NEW_USER;
  reserveTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  reserveTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  reserveTx.obj.toWalletId = AccountBuffer::ID_RESERVE;
  reserveTx.obj.amount = reserveAmount;
  reserveTx.obj.fee = static_cast<int64_t>(chainConfig.minFeePerTransaction);
  reserveTx.obj.meta = reserveAccount.ltsToString();
  reserveTx.signatures.push_back(signTx(genesisKey, reserveTx.obj));
  genesis.block.signedTxes.push_back(reserveTx);

  genesis.hash = validator.calculateHash(genesis.block);
  return genesis;
}

} // namespace

TEST(ValidatorTest, SystemCheckpoint_RoundTrip) {
  Validator::SystemCheckpoint checkpoint;
  checkpoint.config = makeChainConfig(12345);
  checkpoint.genesis = makeUserAccount("pk", 0);

  std::string serialized = checkpoint.ltsToString();
  Validator::SystemCheckpoint parsed;
  EXPECT_TRUE(parsed.ltsFromString(serialized));
  EXPECT_EQ(parsed.config.genesisTime, checkpoint.config.genesisTime);
  EXPECT_EQ(parsed.config.slotDuration, checkpoint.config.slotDuration);
  EXPECT_EQ(parsed.config.slotsPerEpoch, checkpoint.config.slotsPerEpoch);
  EXPECT_EQ(parsed.config.maxPendingTransactions, checkpoint.config.maxPendingTransactions);
  EXPECT_EQ(parsed.config.maxTransactionsPerBlock, checkpoint.config.maxTransactionsPerBlock);
  EXPECT_EQ(parsed.config.minFeePerTransaction, checkpoint.config.minFeePerTransaction);
  EXPECT_EQ(parsed.config.checkpoint.minBlocks, checkpoint.config.checkpoint.minBlocks);
  EXPECT_EQ(parsed.config.checkpoint.minAgeSeconds, checkpoint.config.checkpoint.minAgeSeconds);
  EXPECT_EQ(parsed.genesis.wallet.publicKeys, checkpoint.genesis.wallet.publicKeys);
  EXPECT_EQ(parsed.genesis.wallet.minSignatures, checkpoint.genesis.wallet.minSignatures);
  EXPECT_EQ(parsed.genesis.wallet.mBalances, checkpoint.genesis.wallet.mBalances);
}

TEST(ValidatorTest, CalculateHash_DeterministicAndSensitive) {
  Validator validator;

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

TEST(ValidatorTest, AddBlock_FailsOnGenesisHashMismatch) {
  Validator validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  Validator::BlockChainConfig chainConfig = makeChainConfig(1000);

  Ledger::ChainNode genesis = makeGenesisBlock(validator, chainConfig, genesisKey, feeKey, reserveKey);
  genesis.hash = "bad-hash";

  auto result = validator.addBlock(genesis, true);
  EXPECT_TRUE(result.isError());
  EXPECT_NE(result.error().message.find("Genesis block hash validation failed"), std::string::npos);
}

TEST(ValidatorTest, AddBlock_AddsValidGenesisBlock) {
  Validator validator;

  auto genesisKey = makeKeyPair();
  auto feeKey = makeKeyPair();
  auto reserveKey = makeKeyPair();
  Validator::BlockChainConfig chainConfig = makeChainConfig(1000);

  consensus::Ouroboros::Config consensusConfig;
  consensusConfig.genesisTime = 0;
  consensusConfig.timeOffset = 0;
  consensusConfig.slotDuration = 1;
  consensusConfig.slotsPerEpoch = 10;
  validator.initConsensus(consensusConfig);

  std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pp-ledger-validator-test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(validator, chainConfig, genesisKey, feeKey, reserveKey);

  auto result = validator.addBlock(genesis, true);
  EXPECT_TRUE(result.isOk());
  EXPECT_EQ(validator.getNextBlockId(), 1u);

  std::filesystem::remove_all(tempDir, ec);
}
