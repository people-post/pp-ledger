#include "Genesis.h"
#include "Domains.h"

#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "RecordHandler.h"
#include "TxFees.h"
#include "../client/AccountAttachment.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>

namespace pp::chain_genesis {

namespace {

/** The minimum fee of `tx` under `config` (the fee bills the account's custom meta). */
pp::Roe<uint64_t> minimumFee(const BlockChainConfig &config, const Ledger::TxNewUser &tx) {
  auto fee = minimumFeeFor(config, Ledger::TypedTx(tx), /*afterGenesis=*/false);
  if (!fee) {
    return pp::Error(fee.error().message);
  }
  return fee.value();
}

Ledger::Record newUserRecord(const Ledger::TxNewUser &tx) {
  Ledger::Record rec;
  rec.type = Ledger::T_NEW_USER;
  rec.data = utl::binaryPack(tx);
  return rec;
}

/** A system account's record, its fee the minimum. */
pp::Roe<std::pair<Ledger::Record, uint64_t>> systemAccount(const BlockChainConfig &config, uint64_t id,
                                                           const UserAccount &account, const std::string &label) {
  Ledger::TxNewUser tx;
  tx.fromWalletId = AccountBuffer::ID_GENESIS;
  tx.toWalletId = id;
  auto it = account.wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  tx.amount = it == account.wallet.mBalances.end() ? 0 : static_cast<uint64_t>(it->second);
  tx.meta = account.ltsToString();
  auto fee = minimumFee(config, tx);
  if (!fee) {
    return pp::Error("Failed to calculate the " + label + " fee: " + fee.error().message);
  }
  tx.fee = fee.value();
  return std::make_pair(newUserRecord(tx), tx.fee);
}

/** The genesis miners' records and what they take from the supply (stakes and fees). */
pp::Roe<std::pair<std::vector<Ledger::Record>, int64_t>> minerRecords(const BlockChainConfig &config,
                                                                      const std::vector<Miner> &miners) {
  std::vector<Ledger::Record> records;
  int64_t total = 0;
  std::set<uint64_t> seen;
  for (const auto &miner : miners) {
    if (miner.id < AccountBuffer::ID_FIRST_ISSUED || miner.id >= AccountBuffer::ID_FIRST_USER) {
      return pp::Error("Genesis miner id " + std::to_string(miner.id) + " must be in the issued range [" +
                       std::to_string(AccountBuffer::ID_FIRST_ISSUED) + ", " +
                       std::to_string(AccountBuffer::ID_FIRST_USER) + ")");
    }
    if (!seen.insert(miner.id).second) {
      return pp::Error("Duplicate genesis miner id " + std::to_string(miner.id));
    }
    if (miner.publicKeys.empty() || miner.minSignatures == 0 || miner.minSignatures > miner.publicKeys.size()) {
      return pp::Error("Genesis miner " + std::to_string(miner.id) +
                       " needs public keys and 1 <= minSignatures <= key count");
    }
    if (miner.stake == 0 || miner.stake > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return pp::Error("Genesis miner " + std::to_string(miner.id) + " needs a positive stake");
    }
    UserAccount account;
    account.wallet.mBalances[AccountBuffer::ID_GENESIS] = static_cast<int64_t>(miner.stake);
    account.wallet.publicKeys = miner.publicKeys;
    account.wallet.minSignatures = miner.minSignatures;
    account.wallet.miner.registered = true; // a new chain's committee: registered from block 0
    account.wallet.keyType = Crypto::TK_ML_DSA_65;
    account.meta = AccountAttachment::emptySerialized();

    Ledger::TxNewUser tx;
    tx.fromWalletId = AccountBuffer::ID_GENESIS;
    tx.toWalletId = miner.id;
    tx.amount = miner.stake;
    tx.meta = account.ltsToString();
    auto fee = minimumFee(config, tx);
    if (!fee) {
      return pp::Error("Failed to calculate the genesis miner fee: " + fee.error().message);
    }
    tx.fee = fee.value();
    // All non-negative: overflow-checked sum (portable; no compiler builtins).
    const int64_t add = static_cast<int64_t>(miner.stake);
    const int64_t feeAmount = static_cast<int64_t>(tx.fee);
    if (add > std::numeric_limits<int64_t>::max() - total - feeAmount) {
      return pp::Error("Genesis miner stakes overflow");
    }
    total += add + feeAmount;
    records.push_back(newUserRecord(tx));
  }
  return std::make_pair(std::move(records), total);
}

} // namespace

BlockChainConfig defaultConfig(const std::string &networkId) {
  BlockChainConfig config;
  config.networkId = networkId;
  config.slotDuration = defaults::kSlotDuration;
  config.slotsPerEpoch = defaults::kSlotsPerEpoch;
  config.maxCustomMetaSize = defaults::kMaxCustomMetaSize;
  config.maxTransactionsPerBlock = defaults::kMaxTransactionsPerBlock;
  config.maxBlockBytes = defaults::kMaxBlockBytes;
  config.minFeeCoefficients.assign(std::begin(defaults::kMinFeeCoefficients), std::end(defaults::kMinFeeCoefficients));
  config.freeCustomMetaSize = static_cast<uint32_t>(defaults::kFreeCustomMetaSize);
  config.checkpoint.minBlocks = defaults::kCheckpointMinBlocks;
  config.checkpoint.minAgeSeconds = defaults::kCheckpointMinAgeSeconds;
  config.maxValidationTimespanSeconds = defaults::kMaxValidationTimespanSeconds;
  config.heartbeatSlots = defaults::kSlotsPerEpoch;
  config.minerDueAfterSlots = (defaults::kMinerDueSeconds + defaults::kSlotDuration - 1) / defaults::kSlotDuration;
  config.minerMaxMissedSlots = defaults::kMinerMaxMissedSlots;
  return config;
}

uint64_t defaultRecoveryDelayEpochs(const BlockChainConfig &config) {
  const uint64_t epochSeconds = config.slotDuration * config.slotsPerEpoch;
  if (epochSeconds == 0) {
    return 1;
  }
  return std::max<uint64_t>(1, (defaults::kRecoveryDelaySeconds + epochSeconds - 1) / epochSeconds);
}

uint64_t defaultMinerStake(size_t withoutStake) {
  return withoutStake == 0 ? 0 : AccountBuffer::INITIAL_TOKEN_SUPPLY / defaults::kGenesisMinerStakeShare / withoutStake;
}

UserAccount accountOf(const AccountKeys &keys, int64_t balance, const std::string &meta) {
  UserAccount account;
  account.wallet.mBalances[AccountBuffer::ID_GENESIS] = balance;
  account.wallet.publicKeys = keys.publicKeys;
  account.wallet.minSignatures =
      keys.minSignatures != 0
          ? keys.minSignatures
          : static_cast<uint8_t>(std::min<size_t>(keys.publicKeys.size(), std::numeric_limits<uint8_t>::max()));
  account.wallet.keyType = Crypto::TK_ML_DSA_65;
  account.meta = meta;
  return account;
}

pp::Roe<void> checkKeys(const SystemKeys &keys) {
  const std::pair<const char *, const AccountKeys *> accounts[] = {{"genesis", &keys.genesis},
                                                                   {"reserve", &keys.reserve},
                                                                   {"registrar", &keys.registrar},
                                                                   {"fee", &keys.fee},
                                                                   {"recycle", &keys.recycle}};
  for (const auto &[name, account] : accounts) {
    if (account->publicKeys.empty()) {
      return pp::Error(std::string("System account '") + name + "' needs public keys");
    }
    for (const auto &pk : account->publicKeys) {
      if (pk.size() != utl::kMlDsaPublicKeyBytes) {
        return pp::Error(std::string("System account '") + name + "' has a malformed public key");
      }
    }
    if (account->minSignatures > account->publicKeys.size()) {
      return pp::Error(std::string("System account '") + name + "' minSignatures exceeds its key count");
    }
  }
  // The genesis (admin) account is M-of-N from the start: the same shape its renewals and config
  // updates are held to (at least 3 keys, 2 signatures).
  if (auto shape = chain_tx::validateGenesisWalletShape(accountOf(keys.genesis, 0, "").wallet); !shape) {
    return pp::Error("systemAccounts.genesis: " + shape.error().message);
  }
  return {};
}

SystemNames defaultSystemNames() { return SystemNames{"PPL", "RSV", "REG", "FEE", "RCY"}; }

pp::Roe<void> checkNames(const SystemNames &names) {
  const std::pair<const char *, const std::string *> named[] = {{"genesis", &names.genesis},
                                                                {"reserve", &names.reserve},
                                                                {"registrar", &names.registrar},
                                                                {"fee", &names.fee},
                                                                {"recycle", &names.recycle}};
  std::set<std::string> seen;
  for (const auto &[account, name] : named) {
    if (name->empty()) {
      continue;
    }
    if (auto valid = chain_domain::validateFor(*name, AccountIds::ID_GENESIS); !valid) {
      return pp::Error(std::string("systemNames.") + account + ": " + valid.error().message);
    }
    if (!seen.insert(*name).second) {
      return pp::Error(std::string("systemNames.") + account + ": " + *name + " is another system account's");
    }
  }
  return {};
}

namespace {

UserAccount named(UserAccount account, const std::string &domain) {
  account.wallet.domain = domain;
  return account;
}

} // namespace

pp::Roe<std::vector<Ledger::Record>> records(const Plan &plan) {
  if (auto keysOk = checkKeys(plan.keys); !keysOk) {
    return keysOk.error();
  }
  if (auto namesOk = checkNames(plan.names); !namesOk) {
    return namesOk.error();
  }
  const BlockChainConfig &config = plan.config;

  GenesisAccountMeta gm;
  gm.config = config;
  gm.genesis = named(accountOf(plan.keys.genesis, 0, AccountAttachment::emptySerialized()), plan.names.genesis);
  gm.authority = plan.authority;
  gm.authority.beacon.networkId = config.networkId;
  // The genesis record carries the authority, so the admin keys that sign it sign the generation-1
  // beacon certificate too.
  if (auto valid = chain_tx::validateGenesisAuthority(gm.authority, gm.genesis.wallet, config.networkId); !valid) {
    return pp::Error(valid.error().message);
  }
  Ledger::TxGenesis txGenesis;
  txGenesis.fee = 0;
  txGenesis.meta = gm.ltsToString();
  Ledger::Record genesisRecord;
  genesisRecord.type = Ledger::T_GENESIS;
  genesisRecord.data = utl::binaryPack(txGenesis);

  // System accounts. Fee comes first: the other records pay their fees into it. Fee, registrar and
  // recycle start empty; reserve takes the rest of the supply (reserve funds the registrar later).
  const std::string emptyMeta = AccountAttachment::emptySerialized();
  auto fee = systemAccount(config, AccountBuffer::ID_FEE, named(accountOf(plan.keys.fee, 0, emptyMeta), plan.names.fee),
                           "fee account");
  auto registrar = systemAccount(config, AccountBuffer::ID_REGISTRAR,
                                 named(accountOf(plan.keys.registrar, 0, emptyMeta), plan.names.registrar), "registrar");
  auto recycle = systemAccount(config, AccountBuffer::ID_RECYCLE,
                               named(accountOf(plan.keys.recycle, 0, emptyMeta), plan.names.recycle), "recycle account");
  for (const auto *made : {&fee, &registrar, &recycle}) {
    if (!*made) {
      return made->error();
    }
  }
  auto miners = minerRecords(config, plan.miners);
  if (!miners) {
    return miners.error();
  }

  // The reserve's fee bills its custom meta only, not its balance.
  Ledger::TxNewUser probe;
  probe.meta = named(accountOf(plan.keys.reserve, 0, emptyMeta), plan.names.reserve).ltsToString();
  auto reserveFee = minimumFee(config, probe);
  if (!reserveFee) {
    return pp::Error("Failed to calculate the reserve fee: " + reserveFee.error().message);
  }
  const int64_t otherCosts = static_cast<int64_t>(fee.value().second + registrar.value().second +
                                                  recycle.value().second) +
                             miners.value().second;
  const int64_t reserveAmount = static_cast<int64_t>(AccountBuffer::INITIAL_TOKEN_SUPPLY) - otherCosts -
                                static_cast<int64_t>(reserveFee.value());
  if (reserveAmount < 0) {
    return pp::Error("Initial token supply is insufficient for genesis fees and stakes");
  }
  Ledger::TxNewUser reserveTx;
  reserveTx.fromWalletId = AccountBuffer::ID_GENESIS;
  reserveTx.toWalletId = AccountBuffer::ID_RESERVE;
  reserveTx.amount = static_cast<uint64_t>(reserveAmount);
  reserveTx.fee = reserveFee.value();
  reserveTx.meta = named(accountOf(plan.keys.reserve, reserveAmount, emptyMeta), plan.names.reserve).ltsToString();

  std::vector<Ledger::Record> out;
  out.push_back(std::move(genesisRecord));
  out.push_back(std::move(fee.value().first));
  out.push_back(newUserRecord(reserveTx));
  out.push_back(std::move(registrar.value().first));
  out.push_back(std::move(recycle.value().first));
  for (auto &miner : miners.value().first) {
    out.push_back(std::move(miner));
  }
  return out;
}

pp::Roe<GenesisAccountMeta> metaOf(const Ledger::Record &genesisRecord) {
  if (genesisRecord.type != Ledger::T_GENESIS) {
    return pp::Error("Not a genesis record (type " + std::to_string(genesisRecord.type) + ")");
  }
  auto tx = utl::binaryUnpack<Ledger::TxGenesis>(genesisRecord.data);
  if (!tx) {
    return pp::Error("The genesis record does not decode: " + tx.error().message);
  }
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.value().meta)) {
    return pp::Error("The genesis record's meta does not decode");
  }
  return gm;
}

} // namespace pp::chain_genesis
