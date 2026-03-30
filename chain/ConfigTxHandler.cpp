#include "ConfigTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "Types.h"

namespace pp {

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

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "System update transaction must use genesis wallet (ID_GENESIS -> "
        "ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "System update transaction must have amount 0");
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

chain_tx::Roe<void> ConfigTxHandler::applyConfigUpdate(
    const Ledger::TxConfig &tx, const TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isStrictMode) {
  return applyConfigUpdateCore(tx, log(), ctx.optChainConfig, bank, blockId,
                               isStrictMode, false, nullptr);
}

chain_tx::Roe<void> ConfigTxHandler::applyConfigUpdate(
    const Ledger::TxConfig &tx, TxContext &ctx, AccountBuffer &bank,
    uint64_t blockId, bool isStrictMode, bool commitOptChainConfig) {
  std::optional<BlockChainConfig> *commitTarget =
      commitOptChainConfig ? &ctx.optChainConfig : nullptr;
  return applyConfigUpdateCore(tx, log(), ctx.optChainConfig, bank, blockId,
                               isStrictMode, commitOptChainConfig,
                               commitTarget);
}

} // namespace pp
