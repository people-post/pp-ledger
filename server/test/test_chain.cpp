#include "../Chain.h"
#include "../AccountBuffer.h"
#include "../../lib/Utilities.h"
#include "../../lib/BinaryPack.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace pp;

namespace {

Chain::BlockChainConfig makeChainConfig(int64_t genesisTime) {
  Chain::BlockChainConfig cfg;
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

  Client::UserAccount recycleAccount = makeUserAccount(recycleKey.publicKey, 0);
  recycleAccount.meta = "Account for recycling write-off balances";
  Ledger::SignedData<Ledger::Transaction> recycleTx;
  recycleTx.obj.type = Ledger::Transaction::T_NEW_USER;
  recycleTx.obj.tokenId = AccountBuffer::ID_GENESIS;
  recycleTx.obj.fromWalletId = AccountBuffer::ID_GENESIS;
  recycleTx.obj.toWalletId = AccountBuffer::ID_RECYCLE;
  recycleTx.obj.amount = 0;
  recycleTx.obj.fee = static_cast<int64_t>(chainConfig.minFeePerTransaction);
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
  EXPECT_EQ(parsed.config.maxPendingTransactions, gm.config.maxPendingTransactions);
  EXPECT_EQ(parsed.config.maxTransactionsPerBlock, gm.config.maxTransactionsPerBlock);
  EXPECT_EQ(parsed.config.minFeePerTransaction, gm.config.minFeePerTransaction);
  EXPECT_EQ(parsed.config.checkpoint.minBlocks, gm.config.checkpoint.minBlocks);
  EXPECT_EQ(parsed.config.checkpoint.minAgeSeconds, gm.config.checkpoint.minAgeSeconds);
  EXPECT_EQ(parsed.genesis.wallet.publicKeys, gm.genesis.wallet.publicKeys);
  EXPECT_EQ(parsed.genesis.wallet.minSignatures, gm.genesis.wallet.minSignatures);
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

  Ledger::ChainNode genesis = makeGenesisBlock(validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);
  genesis.hash = "bad-hash";

  auto result = validator.addBlock(genesis, true);
  EXPECT_TRUE(result.isError());
  EXPECT_NE(result.error().message.find("Genesis block hash validation failed"), std::string::npos);
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

  std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "pp-ledger-chain-test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  ASSERT_FALSE(ec);

  Ledger::InitConfig ledgerConfig;
  ledgerConfig.workDir = tempDir.string();
  ledgerConfig.startingBlockId = 0;
  auto initResult = validator.initLedger(ledgerConfig);
  ASSERT_TRUE(initResult.isOk());

  Ledger::ChainNode genesis = makeGenesisBlock(validator, chainConfig, genesisKey, feeKey, reserveKey, recycleKey);

  auto result = validator.addBlock(genesis, true);
  EXPECT_TRUE(result.isOk());
  EXPECT_EQ(validator.getNextBlockId(), 1u);

  std::filesystem::remove_all(tempDir, ec);
}
