#include "GenesisTxHandler.h"
#include "AccountBuffer.h"
#include "ErrorCodes.h"
#include "Types.h"

namespace pp {

chain_tx::Roe<void> GenesisTxHandler::applyGenesisInit(
    const Ledger::Transaction &tx, TxContext &ctx) {
  log().info << "Processing system initialization transaction";

  if (tx.fromWalletId != AccountBuffer::ID_GENESIS ||
      tx.toWalletId != AccountBuffer::ID_GENESIS) {
    return chain_tx::TxError(
        chain_err::E_TX_VALIDATION,
        "System init transaction must use genesis wallet (ID_GENESIS -> "
        "ID_GENESIS)");
  }
  if (tx.amount != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "System init transaction must have amount 0");
  }
  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "System init transaction must have fee 0");
  }

  GenesisAccountMeta gm;
  if (!gm.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE,
                             "Failed to deserialize checkpoint config: " +
                                 tx.meta);
  }

  ctx.optChainConfig = gm.config;

  auto config = ctx.consensus.getConfig();

  if (config.genesisTime == 0) {
    config.genesisTime = ctx.optChainConfig.value().genesisTime;
  } else if (ctx.optChainConfig.value().genesisTime != config.genesisTime) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis time mismatch");
  }
  config.slotDuration = ctx.optChainConfig.value().slotDuration;
  config.slotsPerEpoch = ctx.optChainConfig.value().slotsPerEpoch;
  ctx.consensus.init(config);

  AccountBuffer::Account genesisAccount;
  genesisAccount.id = AccountBuffer::ID_GENESIS;
  genesisAccount.blockId = 0;
  genesisAccount.wallet = gm.genesis.wallet;
  auto roeAddGenesis = ctx.bank.add(genesisAccount);
  if (!roeAddGenesis) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to add genesis account to buffer: " +
            roeAddGenesis.error().message);
  }

  log().info << "System initialized";
  log().info << "  Version: " << gm.VERSION;
  log().info << "  Config: " << ctx.optChainConfig.value();
  log().info << "  Genesis: " << gm.genesis;

  return {};
}

} // namespace pp
