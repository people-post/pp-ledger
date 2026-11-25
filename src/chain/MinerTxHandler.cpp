#include "chain/MinerTxHandler.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountPolicy.h"
#include "ledger/chain/ErrorCodes.h"
#include "ledger/chain/TxFees.h"
#include "ledger/lib/common/Utilities.h"

#include <algorithm>

#include <limits>
#include <string>
#include <variant>

namespace pp {

chain_tx::Roe<const Ledger::TxMiner*> MinerTxHandler::expect(const Ledger::TypedTx& tx) const {
  switch (kind_) {
    case Kind::Register:
      if (const auto* p = std::get_if<Ledger::TxMinerRegister>(&tx)) {
        return static_cast<const Ledger::TxMiner*>(p);
      }
      break;
    case Kind::Revoke:
      if (const auto* p = std::get_if<Ledger::TxMinerRevoke>(&tx)) {
        return static_cast<const Ledger::TxMiner*>(p);
      }
      break;
    case Kind::ProducerKey:
      if (const auto* p = std::get_if<Ledger::TxMinerProducerKey>(&tx)) {
        return static_cast<const Ledger::TxMiner*>(p);
      }
      break;
  }
  return chain_tx::TxError(chain_err::E_INTERNAL, "Expected a miner registration transaction");
}

chain_tx::Roe<uint64_t> MinerTxHandler::getSignerAccountId(const Ledger::TypedTx& tx, uint64_t /*slotLeaderId*/) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  return p.value()->walletId;
}

chain_tx::Roe<bool> MinerTxHandler::matchesWalletForIndex(const Ledger::TypedTx& tx, uint64_t walletId) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  return p.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>> MinerTxHandler::getIdempotencyKey(
    const Ledger::TypedTx& tx) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  if (p.value()->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(p.value()->walletId, p.value()->idempotentId));
}

chain_tx::Roe<void> MinerTxHandler::applyBuffer(const Ledger::TypedTx& tx, AccountBuffer& bank,
                                                const BufferApplyContext& c) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  const Ledger::TxMiner& m = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, m.idempotentId, m.walletId, m.validationTsMin,
                                                  m.validationTsMax, c.effectiveSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  if (auto seeded = chain_tx::seedCommittedAccount(bank, c.ctx.bank, m.walletId); !seeded) {
    return seeded;
  }
  if (auto seeded = chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, m.fee); !seeded) {
    return seeded;
  }
  return applyMiner(tx, m, c.ctx, bank, c.effectiveSlot, chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> MinerTxHandler::applyBlock(const Ledger::TypedTx& tx, AccountBuffer& bank,
                                               const BlockApplyContext& c) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  const Ledger::TxMiner& m = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, m.idempotentId, m.walletId, m.validationTsMin,
                                                  m.validationTsMax, c.blockSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  return applyMiner(tx, m, c.ctx, bank, c.blockSlot, c.admissionMode);
}

chain_tx::Roe<void> MinerTxHandler::checkRegisterAllowed(const Ledger::TxMiner& tx, const TxContext& ctx,
                                                         const Client::Wallet& wallet, uint64_t slot) const {
  if (AccountIds::isSystemAccount(tx.walletId)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "System accounts cannot be miners");
  }
  if (wallet.miner.registered) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Account is already a registered miner: " + std::to_string(tx.walletId));
  }
  const BlockChainConfig& config = ctx.optChainConfig.value();
  const uint64_t deregistered = wallet.miner.autoDeregisteredEpoch;
  if (deregistered != Client::MinerState::kNever && config.minerReregisterCooldownEpochs > 0) {
    const uint64_t epoch = ctx.consensus.getEpochFromSlot(slot);
    if (epoch < deregistered || epoch - deregistered < config.minerReregisterCooldownEpochs) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Miner is in its re-registration cool-down until epoch " +
                                   std::to_string(deregistered + config.minerReregisterCooldownEpochs));
    }
  }
  const auto balanceIt = wallet.mBalances.find(AccountBuffer::ID_GENESIS);
  const int64_t balance = balanceIt == wallet.mBalances.end() ? 0 : balanceIt->second;
  const int64_t afterFee = balance - static_cast<int64_t>(std::min<uint64_t>(tx.fee, INT64_MAX));
  if (afterFee < 0 || static_cast<uint64_t>(afterFee) < config.minerMinStake) {
    return chain_tx::TxError(chain_err::E_ACCOUNT_BALANCE,
                             "Miner registration needs a balance of at least minerMinStake (" +
                                 std::to_string(config.minerMinStake) + ") after its fee");
  }
  return {};
}

chain_tx::Roe<void> MinerTxHandler::checkProducerKey(const Ledger::TxMinerProducerKey& tx,
                                                     const Client::Wallet& wallet) const {
  if (AccountIds::isSystemAccount(tx.walletId)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "System accounts cannot be miners");
  }
  if (tx.producerKey.empty()) {
    if (!wallet.miner.hasProducerKey()) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Account " + std::to_string(tx.walletId) + " has no producer key to clear");
    }
    return {};
  }
  if (tx.producerKey.size() != utl::kMlDsaPublicKeyBytes) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A producer key is an ML-DSA-65 public key");
  }
  // A hot key that is also one of the account's keys would put the account's keys back on the host.
  if (std::find(wallet.publicKeys.begin(), wallet.publicKeys.end(), tx.producerKey) != wallet.publicKeys.end()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A producer key cannot be one of the account's own keys");
  }
  if (tx.producerKey == wallet.miner.producerKey) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "That producer key is already in force for account " + std::to_string(tx.walletId));
  }
  return {};
}

chain_tx::Roe<void> MinerTxHandler::applyMiner(const Ledger::TypedTx& typed, const Ledger::TxMiner& tx,
                                               const TxContext& ctx, AccountBuffer& bank, uint64_t slot,
                                               chain_block::BlockAdmissionMode admissionMode) const {
  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain config required for miner registration");
  }
  const BlockChainConfig& config = ctx.optChainConfig.value();
  if (chain_block::admissionTxStrict(admissionMode)) {
    if (!ctx.fnBillableCustomMetaSizeForFee.has_value()) {
      return chain_tx::TxError(chain_err::E_INTERNAL, "Fee-meta size extractor not configured on TxContext");
    }
    auto metaFee = chain_tx::calculateMinimumFeeForTransaction(config, typed, *ctx.fnBillableCustomMetaSizeForFee);
    if (!metaFee) {
      return metaFee.error();
    }
    const uint64_t flat = kind_ == Kind::Register ? config.minerRegistrationFee : 0;
    if (flat > std::numeric_limits<uint64_t>::max() - metaFee.value() || tx.fee < metaFee.value() + flat) {
      const char* what = kind_ == Kind::Register ? "Miner registration"
                         : kind_ == Kind::Revoke ? "Miner revoke"
                                                 : "Miner producer key";
      return chain_tx::TxError(chain_err::E_TX_FEE, std::string(what) + " fee below minimum " +
                                                        std::to_string(metaFee.value() + flat) + ": " +
                                                        std::to_string(tx.fee));
    }
  }

  auto accountRoe = bank.getAccount(tx.walletId);
  if (!accountRoe) {
    if (!chain_block::admissionTxStrict(admissionMode)) {
      // Warming up: the account arrives with its renewal; the fee still reaches a held fee account.
      return chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit miner fee: ");
    }
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.walletId));
  }
  const Client::Wallet wallet = accountRoe.value().wallet;
  Client::MinerState miner = wallet.miner;
  switch (kind_) {
    case Kind::Register:
      if (auto allowed = checkRegisterAllowed(tx, ctx, wallet, slot); !allowed) {
        return allowed;
      }
      miner.registered = true;
      miner.lastProducedSlot = slot; // a new miner is not due straight away
      miner.missedDue = 0;
      miner.autoDeregisteredEpoch = Client::MinerState::kNever;
      break;
    case Kind::Revoke:
      if (!miner.registered) {
        return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                                 "Account is not a registered miner: " + std::to_string(tx.walletId));
      }
      miner.registered = false;
      miner.missedDue = 0;
      break;
    case Kind::ProducerKey: {
      // Registered or not: a miner may certify its key before it registers, and keeps it after a revoke.
      const auto& producer = std::get<Ledger::TxMinerProducerKey>(typed);
      if (auto valid = checkProducerKey(producer, wallet); !valid) {
        return valid;
      }
      miner.producerKey = producer.producerKey;
      break;
    }
  }

  if (tx.fee > 0) {
    if (auto paid = bank.withdrawBalance(tx.walletId, AccountBuffer::ID_GENESIS, static_cast<int64_t>(tx.fee)); !paid) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_BALANCE, "Failed to pay miner fee: " + paid.error().message);
    }
    if (auto credited = chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit miner fee: "); !credited) {
      return credited;
    }
  }
  if (auto set = bank.setMinerState(tx.walletId, miner); !set) {
    return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace pp
