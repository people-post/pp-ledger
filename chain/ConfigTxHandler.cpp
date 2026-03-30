#include "ConfigTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "TxIdempotency.h"
#include "Types.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
ConfigTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                    uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getSignerAccountId: expected TxConfig");
  }
  (void)p;
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<bool>
ConfigTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                       uint64_t walletId) const {
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "matchesWalletForIndex: expected TxConfig");
  }
  (void)p;
  return walletId == AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
ConfigTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "getIdempotencyKey: expected TxConfig");
  }
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(AccountBuffer::ID_GENESIS, p->idempotentId));
}

namespace {

chain_tx::Roe<void> applyConfigUpdateCore(
    const Ledger::TxConfig &tx, logging::Logger &logger,
    const std::optional<BlockChainConfig> &chainConfigBaseline,
    AccountBuffer &bank, uint64_t blockId, bool isStrictMode,
    bool commitOptChainConfig, std::optional<BlockChainConfig> *commitTarget) {
  if (commitOptChainConfig && commitTarget == nullptr) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "commitTarget required when committing chain config");
  }

  if (commitOptChainConfig) {
    logger.info << "Processing system update transaction";
  }

  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "System update transaction must have fee 0");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE,
                             "Failed to deserialize checkpoint config: " +
                                 tx.meta);
  }

  if (gm.genesis.wallet.publicKeys.size() < 3) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account must have at least 3 public keys");
  }

  if (gm.genesis.wallet.minSignatures < 2) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account must have at least 2 signatures");
  }

  if (isStrictMode) {
    if (!chainConfigBaseline.has_value()) {
      return chain_tx::TxError(chain_err::E_STATE_INIT,
                               "Chain config not initialized for strict update");
    }
    if (gm.config.genesisTime != chainConfigBaseline.value().genesisTime) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Genesis time mismatch");
    }

    if (gm.config.slotDuration > chainConfigBaseline.value().slotDuration) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Slot duration cannot be increased");
    }

    if (gm.config.slotsPerEpoch < chainConfigBaseline.value().slotsPerEpoch) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Slots per epoch cannot be decreased");
    }
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, 0,
                          gm.genesis.wallet.mBalances)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account balance mismatch");
  }

  bank.remove(AccountBuffer::ID_GENESIS);

  AccountBuffer::Account account;
  account.id = AccountBuffer::ID_GENESIS;
  account.blockId = blockId;
  account.wallet = gm.genesis.wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER,
                             "Failed to add updated genesis account: " +
                                 addResult.error().message);
  }

  if (commitOptChainConfig) {
    *commitTarget = gm.config;
    logger.info << "System updated";
    logger.info << "  Version: " << GenesisAccountMeta::VERSION;
    logger.info << "  Config: " << commitTarget->value();
  }

  return {};
}

} // namespace

chain_tx::Roe<void> ConfigTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BufferApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBuffer: expected TxConfig");
  }
  if (auto idem = chain_tx::validateIdempotencyRules(
          c.ctx.ledger, c.ctx.consensus, c.ctx.optChainConfig, p->idempotentId,
          AccountBuffer::ID_GENESIS, p->validationTsMin, p->validationTsMax,
          c.effectiveSlot, c.isStrictMode);
      !idem) {
    return idem;
  }
  if (auto r =
          bank.seedFromCommittedIfMissing(c.ctx.bank, AccountBuffer::ID_GENESIS);
      !r) {
    return chain_tx::TxError(r.error().code, r.error().message);
  }
  return applyConfigUpdate(*p, c.ctx, bank, c.blockId, true);
}

chain_tx::Roe<void> ConfigTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BlockApplyContext &c) const {
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return chain_tx::TxError(chain_err::E_INTERNAL,
                             "applyBlock: expected TxConfig");
  }
  if (auto idem = chain_tx::validateIdempotencyRules(
          c.ctx.ledger, c.ctx.consensus, c.ctx.optChainConfig, p->idempotentId,
          AccountBuffer::ID_GENESIS, p->validationTsMin, p->validationTsMax,
          c.blockSlot, c.isStrictMode);
      !idem) {
    return idem;
  }
  return applyConfigUpdate(*p, c.ctx, bank, c.blockId, c.isStrictMode,
                           true);
}

chain_tx::Roe<void> ConfigTxHandler::applyConfigUpdate(
    const Ledger::TxConfig &tx, const TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isStrictMode) const {
  return applyConfigUpdateCore(tx, log(), ctx.optChainConfig, bank, blockId,
                               isStrictMode, false, nullptr);
}

chain_tx::Roe<void> ConfigTxHandler::applyConfigUpdate(
    const Ledger::TxConfig &tx, TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isStrictMode, bool commitOptChainConfig) const {
  std::optional<BlockChainConfig> *commitTarget =
      commitOptChainConfig ? &ctx.optChainConfig : nullptr;
  return applyConfigUpdateCore(tx, log(), ctx.optChainConfig, bank, blockId,
                               isStrictMode, commitOptChainConfig,
                               commitTarget);
}

} // namespace pp
