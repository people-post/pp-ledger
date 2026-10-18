#include "DomainAssignTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxFees.h"
#include "TxTyped.h"

#include <limits>
#include <string>

namespace pp {

chain_tx::Roe<uint64_t> DomainAssignTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                                  uint64_t /*slotLeaderId*/) const {
  auto p = chain_tx::expectTx<Ledger::TxDomainAssign>(tx, "getSignerAccountId", "TxDomainAssign");
  if (!p) {
    return p.error();
  }
  return AccountBuffer::ID_REGISTRAR;
}

chain_tx::Roe<bool> DomainAssignTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const {
  auto p = chain_tx::expectTx<Ledger::TxDomainAssign>(tx, "matchesWalletForIndex", "TxDomainAssign");
  if (!p) {
    return p.error();
  }
  return walletId == AccountBuffer::ID_REGISTRAR || walletId == p.value()->walletId;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
DomainAssignTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto p = chain_tx::expectTx<Ledger::TxDomainAssign>(tx, "getIdempotencyKey", "TxDomainAssign");
  if (!p) {
    return p.error();
  }
  if (p.value()->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(AccountBuffer::ID_REGISTRAR, p.value()->idempotentId));
}

chain_tx::Roe<void> DomainAssignTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                       const BufferApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxDomainAssign>(tx, "applyBuffer", "TxDomainAssign");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, AccountBuffer::ID_REGISTRAR,
                                                  r.validationTsMin, r.validationTsMax, c.effectiveSlot,
                                                  c.admissionMode, /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  for (uint64_t id : {AccountBuffer::ID_REGISTRAR, r.walletId}) {
    if (auto seeded = chain_tx::seedCommittedAccount(bank, c.ctx.bank, id); !seeded) {
      return seeded;
    }
  }
  if (auto seeded = chain_tx::seedFeeAccountIfNeeded(bank, c.ctx.bank, r.fee); !seeded) {
    return seeded;
  }
  return apply(r, c.ctx, bank, &c.ctx.bank, c.blockId, chain_block::BlockAdmissionMode::Full);
}

chain_tx::Roe<void> DomainAssignTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                      const BlockApplyContext &c) const {
  auto p = chain_tx::expectTx<Ledger::TxDomainAssign>(tx, "applyBlock", "TxDomainAssign");
  if (!p) {
    return p.error();
  }
  const auto &r = *p.value();
  if (auto idem = validateIdempotencyUsingContext(c.ctx, r.idempotentId, AccountBuffer::ID_REGISTRAR,
                                                  r.validationTsMin, r.validationTsMax, c.blockSlot,
                                                  c.admissionMode, /*requireNonZeroId=*/true);
      !idem) {
    return idem;
  }
  return apply(r, c.ctx, bank, nullptr, c.blockId, c.admissionMode);
}

chain_tx::Roe<void> DomainAssignTxHandler::apply(const Ledger::TxDomainAssign &tx, const TxContext &ctx,
                                                 AccountBuffer &bank, const AccountBuffer *committed, uint64_t blockId,
                                                 chain_block::BlockAdmissionMode admissionMode) const {
  if (blockId == 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE, "The genesis block names its accounts in their records");
  }
  const bool strict = chain_block::admissionTxStrict(admissionMode);
  if (strict) {
    if (auto feeGate = chain_tx::requireMinimumFee(ctx.optChainConfig, ctx.fnBillableCustomMetaSizeForFee,
                                                   Ledger::TypedTx(tx), tx.fee,
                                                   "Chain config required for a domain assignment",
                                                   "Domain assignment fee below minimum: ");
        !feeGate) {
      return feeGate;
    }
  }
  if (!AccountIds::isIssuedAccount(tx.walletId)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "The registrar gives domains to issued accounts only: " + std::to_string(tx.walletId));
  }
  auto account = bank.getAccount(tx.walletId);
  if (!account) {
    if (!strict) {
      // Warming up: the account arrives with its renewal, its domain with it.
      return chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the domain fee: ");
    }
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Account not found: " + std::to_string(tx.walletId));
  }
  if (!account.value().wallet.domain.empty()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Account " + std::to_string(tx.walletId) +
                                                             " has a domain already: " +
                                                             account.value().wallet.domain);
  }
  if (auto valid = chain_tx::validateNewDomain(tx.domain, tx.walletId, bank, committed); !valid) {
    return valid;
  }
  if (tx.fee > 0) {
    if (auto paid = bank.withdrawBalance(AccountBuffer::ID_REGISTRAR, AccountBuffer::ID_GENESIS,
                                         static_cast<int64_t>(std::min<uint64_t>(tx.fee, INT64_MAX)));
        !paid) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_BALANCE,
                               "The registrar cannot pay the domain's fee: " + paid.error().message);
    }
    if (auto credited = chain_tx::creditFeeToFeeAccount(bank, tx.fee, "Failed to credit the domain fee: ");
        !credited) {
      return credited;
    }
  }
  if (auto set = bank.setDomain(tx.walletId, tx.domain); !set) {
    return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace pp
