#include "ConfigTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxTyped.h"
#include "Types.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t>
ConfigTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                    uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe =
      chain_tx::expectTx<Ledger::TxConfig>(tx, "getSignerAccountId", "TxConfig");
  if (!pRoe) {
    return pRoe.error();
  }
  (void)pRoe.value();
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<bool>
ConfigTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                       uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxConfig>(tx, "matchesWalletForIndex",
                                                   "TxConfig");
  if (!pRoe) {
    return pRoe.error();
  }
  (void)pRoe.value();
  return walletId == AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
ConfigTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxConfig>(tx, "getIdempotencyKey", "TxConfig");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(
      std::make_pair(AccountBuffer::ID_GENESIS, p->idempotentId));
}

namespace {

/** What a config update may change, relative to the active config. */
chain_tx::Roe<void> validateConfigChange(const BlockChainConfig &next,
                                         const std::optional<BlockChainConfig> &current) {
  if (!current.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT,
                             "Chain config not initialized for Full-mode update");
  }
  if (next.genesisTime != current->genesisTime) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Genesis time mismatch");
  }
  if (next.slotDuration > current->slotDuration) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Slot duration cannot be increased");
  }
  if (next.slotsPerEpoch < current->slotsPerEpoch) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Slots per epoch cannot be decreased");
  }
  return {};
}

} // namespace

chain_tx::Roe<void> ConfigTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                 AccountBuffer &bank,
                                                 const BufferApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxConfig>(tx, "applyBuffer", "TxConfig");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, AccountBuffer::ID_GENESIS, p->validationTsMin,
          p->validationTsMax, c.effectiveSlot, c.admissionMode);
      !idem) {
    return idem;
  }
  if (auto seeded = chain_tx::seedCommittedAccount(
          bank, c.ctx.bank, AccountBuffer::ID_GENESIS);
      !seeded) {
    return seeded;
  }
  auto config = applyConfigUpdate(*p, c.ctx, bank, c.blockId,
                                  c.ctx.consensus.getEpochFromSlot(c.effectiveSlot),
                                  chain_block::BlockAdmissionMode::Full);
  if (!config) {
    return config.error();
  }
  return {};
}

chain_tx::Roe<void> ConfigTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                AccountBuffer &bank,
                                                const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxConfig>(tx, "applyBlock", "TxConfig");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (auto idem = validateIdempotencyUsingContext(
          c.ctx, p->idempotentId, AccountBuffer::ID_GENESIS, p->validationTsMin,
          p->validationTsMax, c.blockSlot, c.admissionMode);
      !idem) {
    return idem;
  }
  const uint64_t epoch = c.ctx.consensus.getEpochFromSlot(c.blockSlot);
  auto config = applyConfigUpdate(*p, c.ctx, bank, c.blockId, epoch, c.admissionMode);
  if (!config) {
    return config.error();
  }
  // Config changes apply at the next epoch boundary: everyone sees a change
  // for the rest of its epoch before it binds. Chain activates it.
  const uint64_t activationEpoch = epoch + 1;
  c.ctx.pendingChainConfig = PendingChainConfig{activationEpoch, std::move(config.value())};
  log().info << "Config update accepted; applies from epoch " << activationEpoch;
  return {};
}

chain_tx::Roe<BlockChainConfig> ConfigTxHandler::applyConfigUpdate(
    const Ledger::TxConfig &tx, const TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, uint64_t epoch, chain_block::BlockAdmissionMode admissionMode) const {
  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "System update transaction must have fee 0");
  }
  // One pending update at a time, so at most one takes effect per epoch. (A
  // pending update whose epoch has come is in force once that epoch's first
  // block applies.)
  if (ctx.pendingChainConfig.has_value() && epoch < ctx.pendingChainConfig->activationEpoch) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "A config update is already pending (applies from epoch " +
            std::to_string(ctx.pendingChainConfig->activationEpoch) + ")");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE,
                             "Failed to deserialize checkpoint config: " +
                                 tx.meta);
  }

  if (auto shape = chain_tx::validateGenesisWalletShape(gm.genesis.wallet);
      !shape) {
    return shape.error();
  }

  if (chain_block::admissionTxStrict(admissionMode)) {
    if (auto change = validateConfigChange(gm.config, ctx.optChainConfig); !change) {
      return change.error();
    }
  }

  if (!bank.verifyBalance(AccountBuffer::ID_GENESIS, 0, 0,
                          gm.genesis.wallet.mBalances)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis account balance mismatch");
  }

  auto attachmentRoe = chain_tx::validateAndCanonicalizeAttachment(
      gm.genesis.meta,
      [&](uint64_t id) {
        return id == AccountBuffer::ID_GENESIS || bank.hasAccount(id);
      },
      "Genesis account attachment: ");
  if (!attachmentRoe) {
    return attachmentRoe.error();
  }
  gm.genesis.meta = attachmentRoe.value();

  if (auto replaced = chain_tx::replaceGenesisAccount(bank, blockId,
                                                      gm.genesis.wallet);
      !replaced) {
    return replaced.error();
  }
  return gm.config;
}

std::optional<std::string>
ConfigTxHandler::getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
                                            const Ledger::Block & /*block*/) const {
  const auto *p = std::get_if<Ledger::TxConfig>(&tx);
  if (!p) {
    return std::nullopt;
  }
  return p->meta;
}

} // namespace pp
