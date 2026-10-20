#pragma once
// Chains for tests: a genesis with the fixture's system accounts and miner, keys, signed records, and
// blocks sealed by the elected leader at a slot — shared by test_chain and the ledger-scale benchmark
// (projects/load-resilience lr9).

#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Crypto.h"
#include "ledger/lib/common/Utilities.h"
#include "ledger/chain/AccountBuffer.h"
#include "chain/Chain.h"
#include "ledger/client/AccountAttachment.h"
#include "ledger/client/Client.h"
#include "testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <vector>

namespace pp::chain_test {


constexpr uint64_t BYTES_PER_KIB = 1024ULL;

inline uint64_t getFeeCoefficient(const std::vector<uint16_t> &coefficients,
                           size_t index) {
  return index < coefficients.size()
             ? static_cast<uint64_t>(coefficients[index])
             : 0ULL;
}

inline uint64_t
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

inline Chain::BlockChainConfig makeChainConfig(int64_t genesisTime) {
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
  cfg.maxBlockBytes = 4 * 1024 * 1024;
  cfg.finalitySeconds = 3600; // the floor (projects/finality FN001)
  return cfg;
}

inline utl::MlDsaKeyPair makeKeyPair() {
  auto result = utl::mlDsaGenerate();
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

inline Client::UserAccount makeUserAccount(const std::string &publicKey,
                                    int64_t balance) {
  Client::UserAccount account;
  account.wallet.publicKeys = {publicKey};
  account.wallet.minSignatures = 1;
  account.wallet.keyType = Crypto::TK_ML_DSA_65;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.meta = AccountAttachment::emptySerialized();
  return account;
}

inline std::string signMessage(const utl::MlDsaKeyPair &keyPair,
                        const std::string &message) {
  auto result = utl::mlDsaSign(keyPair.privateKey, message);
  EXPECT_TRUE(result.isOk());
  if (!result.isOk()) {
    return {};
  }
  return result.value();
}

template <typename TxT>
inline Ledger::Record makeRecord(uint16_t type, const TxT &tx,
                          const utl::MlDsaKeyPair &signer,
                          const std::string &networkId = {}) {
  Ledger::Record rec;
  rec.type = type;
  rec.data = utl::binaryPack(tx);
  rec.signatures = {signMessage(signer, rec.signingMessage(networkId))};
  return rec;
}

/** The generation-1 beacon signing key of every test chain (tests sign tips with it). */
inline const utl::MlDsaKeyPair &testBeaconKey() {
  static const utl::MlDsaKeyPair key = makeKeyPair();
  return key;
}

/** The fixture's recovery set: one key (1-of-1). */
inline const utl::MlDsaKeyPair &testRecoveryKey() {
  static const utl::MlDsaKeyPair key = makeKeyPair();
  return key;
}

/** A valid genesis authority for `networkId`: a 1-of-1 recovery set (testRecoveryKey()) and testBeaconKey(). */
inline Chain::ChainAuthority makeTestAuthority(const std::string &networkId) {
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
inline Ledger::ChainNode buildUnsealedGenesis(const Chain::BlockChainConfig &chainConfig,
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

inline Ledger::ChainNode makeGenesisBlock(Chain &validator,
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

inline Ledger::ChainNode makeNextBlockAtSlot(
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

inline Ledger::ChainNode makeNextBlock(
    Chain &validator, const Ledger::ChainNode &previous,
    const std::vector<Ledger::Record> &records) {
  return makeNextBlockAtSlot(validator, previous, previous.block.slot + 1,
                             records);
}



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
    consensusConfig.timeOffsetMs = 0;
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


} // namespace pp::chain_test
