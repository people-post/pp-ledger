#include "chain/AccountRecoveryTxHandler.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountPolicy.h"
#include "ledger/chain/ErrorCodes.h"
#include "ledger/chain/TxFees.h"
#include "chain/TxTyped.h"

#include <limits>
#include <string>

namespace pp {

chain_tx::Roe<uint64_t> AccountRecoveryTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                                     uint64_t /*slotLeaderId*/) const {
  // Indexed under the recovered account; the signatures are its recovery set's (Chain checks them).
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "getSignerAccountId", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  return p.value()->walletId;
}

chain_tx::Roe<bool> AccountRecoveryTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                                                    uint64_t walletId) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "matchesWalletForIndex", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  return p.value()->walletId == walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
AccountRecoveryTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "getIdempotencyKey", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  if (p.value()->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(p.value()->walletId, p.value()->idempotentId));
}

chain_tx::Roe<AccountRecoveryAction> AccountRecoveryTxHandler::decodeAction(const Ledger::TxAccountRecovery &tx) {
  AccountRecoveryAction action;
  if (!action.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE, "Failed to deserialize the account recovery");
  }
  switch (action.kind) {
  case AccountRecoveryAction::ANNOUNCE:
    if (action.delaySeconds != 0) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, "An announcement names no delay");
    }
    if (auto valid = chain_tx::validateRecoveryKeys(action.publicKeys, action.minSignatures, "The announced keys");
        !valid) {
      return valid.error();
    }
    return action;
  case AccountRecoveryAction::WITHDRAW:
    if (!action.publicKeys.empty() || action.minSignatures != 0 || action.delaySeconds != 0) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A withdrawal names no keys or delay");
    }
    return action;
  case AccountRecoveryAction::CHANGE:
    return action; // checked against the account and the config on apply (validateRecoverySet)
  default:
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Unknown account recovery action: " + std::to_string(action.kind));
  }
}

chain_tx::Roe<void> AccountRecoveryTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                          const BufferApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "applyBuffer", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, r.walletId, r.validationTsMin,
                                                  r.validationTsMax, c.effectiveSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  if (auto seeded = chain_tx::seedCommittedAccount(bank, c.ctx.bank, r.walletId); !seeded) {
    return seeded;
  }
  if (auto seeded = chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, r.fee); !seeded) {
    return seeded;
  }
  return apply(r, c.ctx, bank, c.blockId, c.effectiveSlot, chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> AccountRecoveryTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                         const BlockApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxAccountRecovery>(tx, "applyBlock", "TxAccountRecovery");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, r.walletId, r.validationTsMin,
                                                  r.validationTsMax, c.blockSlot, c.admissionMode,
                                                  /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  return apply(r, c.ctx, bank, c.blockId, c.blockSlot, c.admissionMode);
}

chain_tx::Roe<void> AccountRecoveryTxHandler::apply(const Ledger::TxAccountRecovery &tx, const TxContext &ctx,
                                                    AccountBuffer &bank, uint64_t blockId, uint64_t slot,
                                                    chain_block::BlockAdmissionMode admissionMode) const {
  if (blockId == 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE, "An account recovery cannot be in the genesis block");
  }
  auto action = decodeAction(tx);
  if (!action) {
    return action.error();
  }
  if (!ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain config required for an account recovery");
  }
  const BlockChainConfig &config = ctx.optChainConfig.value();
  if (chain_block::admissionTxStrict(admissionMode)) {
    if (!ctx.fnBillableCustomMetaSizeForFee.has_value()) {
      return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain config required for an account recovery");
    }
    auto least = chain_tx::calculateMinimumFeeForTransaction(config, Ledger::TypedTx(tx),
                                                             *ctx.fnBillableCustomMetaSizeForFee);
    if (!least) {
      return least.error();
    }
    if (tx.fee < least.value()) {
      return chain_tx::TxError(chain_err::E_TX_FEE, "Account recovery fee below minimum " +
                                                        std::to_string(least.value()) + ": " + std::to_string(tx.fee));
    }
  }
  auto accountRoe = bank.getAccount(tx.walletId);
  if (!accountRoe) {
    if (!chain_block::admissionTxStrict(admissionMode)) {
      // Warming up: the account arrives with its renewal; the fee still reaches a held fee account.
      return chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the recovery fee: ");
    }
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.walletId));
  }
  Client::AccountRecovery recovery = accountRoe.value().wallet.recovery;
  if (!recovery.hasSet()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Account " + std::to_string(tx.walletId) + " has no recovery set");
  }
  const std::string account = "Account " + std::to_string(tx.walletId);
  Client::AccountRecovery next = recovery;
  switch (action.value().kind) {
  case AccountRecoveryAction::ANNOUNCE:
    if (recovery.pending) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, account + " has a recovery announced already (until epoch " +
                                                               std::to_string(recovery.applyEpoch) + ")");
    }
    next.pending = true;
    next.nextPublicKeys = action.value().publicKeys;
    next.nextMinSignatures = action.value().minSignatures;
    next.applyEpoch = chain_tx::recoveryApplyEpoch(config, slot, recovery.delaySeconds);
    next.announcedAt = blockId;
    break;
  case AccountRecoveryAction::WITHDRAW:
    if (!recovery.pending) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, account + " has no recovery announced");
    }
    next.pending = false;
    next.nextPublicKeys.clear();
    next.nextMinSignatures = 0;
    next.applyEpoch = 0;
    next.announcedAt = 0;
    break;
  default: { // CHANGE
    if (recovery.pending) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               account + " has a recovery announced: withdraw it before changing the set");
    }
    next = Client::AccountRecovery{};
    next.publicKeys = action.value().publicKeys;
    next.minSignatures = action.value().minSignatures;
    next.delaySeconds = action.value().delaySeconds;
    if (auto valid = chain_tx::validateRecoverySet(next, tx.walletId, ctx.optChainConfig); !valid) {
      return valid;
    }
    break;
  }
  }
  if (tx.fee > 0) {
    if (auto paid = bank.withdrawBalance(tx.walletId, AccountBuffer::ID_GENESIS, static_cast<int64_t>(
                                                                                     std::min<uint64_t>(tx.fee, INT64_MAX)));
        !paid) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_BALANCE, "The account cannot pay the recovery's fee: " +
                                                                 paid.error().message);
    }
    if (auto credited = chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the recovery fee: ");
        !credited) {
      return credited;
    }
  }
  if (auto set = bank.setRecovery(tx.walletId, next); !set) {
    return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace pp
