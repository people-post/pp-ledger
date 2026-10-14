#include "MinerTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"

#include <limits>
#include <string>
#include <variant>

namespace pp {

chain_tx::Roe<const Ledger::TxMiner *> MinerTxHandler::expect(const Ledger::TypedTx &tx) const {
  if (registering_) {
    if (const auto *p = std::get_if<Ledger::TxMinerRegister>(&tx)) {
      return static_cast<const Ledger::TxMiner *>(p);
    }
  } else if (const auto *p = std::get_if<Ledger::TxMinerRevoke>(&tx)) {
    return static_cast<const Ledger::TxMiner *>(p);
  }
  return chain_tx::TxError(chain_err::E_INTERNAL, "Expected a miner registration transaction");
}

chain_tx::Roe<uint64_t> MinerTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                           uint64_t /*slotLeaderId*/) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  return p.value()->walletId;
}

chain_tx::Roe<bool> MinerTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  return p.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
MinerTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  if (p.value()->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(p.value()->walletId, p.value()->idempotentId));
}

chain_tx::Roe<void> MinerTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                const BufferApplyContext &c) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  const Ledger::TxMiner &m = *p.value();
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
  return applyMiner(m, c.ctx, bank, c.effectiveSlot, chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> MinerTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                               const BlockApplyContext &c) const {
  auto p = expect(tx);
  if (!p) {
    return p.error();
  }
  const Ledger::TxMiner &m = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, m.idempotentId, m.walletId, m.validationTsMin,
                                                  m.validationTsMax, c.blockSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  return applyMiner(m, c.ctx, bank, c.blockSlot, c.admissionMode);
}

chain_tx::Roe<void> MinerTxHandler::checkRegisterAllowed(const Ledger::TxMiner &tx, const TxContext &ctx,
                                                         const Client::Wallet &wallet, uint64_t slot) const {
  if (AccountIds::isSystemAccount(tx.walletId)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "System accounts cannot be miners");
  }
  if (wallet.miner.registered) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Account is already a registered miner: " + std::to_string(tx.walletId));
  }
  const BlockChainConfig &config = ctx.optChainConfig.value();
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

chain_tx::Roe<void> MinerTxHandler::applyMiner(const Ledger::TxMiner &tx, const TxContext &ctx, AccountBuffer &bank,
                                               uint64_t slot, chain_block::BlockAdmissionMode admissionMode) const {
  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain config required for miner registration");
  }
  const BlockChainConfig &config = ctx.optChainConfig.value();
  if (chain_block::admissionTxStrict(admissionMode)) {
    if (!ctx.fnBillableCustomMetaSizeForFee.has_value()) {
      return chain_tx::TxError(chain_err::E_INTERNAL, "Fee-meta size extractor not configured on TxContext");
    }
    auto metaFee = chain_tx::calculateMinimumFeeForTransaction(config, Ledger::TypedTx(Ledger::TxMinerRegister{tx}),
                                                               *ctx.fnBillableCustomMetaSizeForFee);
    if (!metaFee) {
      return metaFee.error();
    }
    const uint64_t flat = registering_ ? config.minerRegistrationFee : 0;
    if (flat > std::numeric_limits<uint64_t>::max() - metaFee.value() || tx.fee < metaFee.value() + flat) {
      return chain_tx::TxError(chain_err::E_TX_FEE,
                               std::string(registering_ ? "Miner registration" : "Miner revoke") +
                                   " fee below minimum " + std::to_string(metaFee.value() + flat) + ": " +
                                   std::to_string(tx.fee));
    }
  }

  auto accountRoe = bank.getAccount(tx.walletId);
  if (!accountRoe) {
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.walletId));
  }
  const Client::Wallet wallet = accountRoe.value().wallet;
  Client::MinerState miner = wallet.miner;
  if (registering_) {
    if (auto allowed = checkRegisterAllowed(tx, ctx, wallet, slot); !allowed) {
      return allowed;
    }
    miner.registered = true;
    miner.lastProducedSlot = slot;  // a new miner is not due straight away
    miner.missedDue = 0;
    miner.autoDeregisteredEpoch = Client::MinerState::kNever;
  } else {
    if (!miner.registered) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Account is not a registered miner: " + std::to_string(tx.walletId));
    }
    miner.registered = false;
    miner.missedDue = 0;
  }

  if (tx.fee > 0) {
    if (auto paid = bank.withdrawBalance(tx.walletId, AccountBuffer::ID_GENESIS, static_cast<int64_t>(tx.fee));
        !paid) {
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
