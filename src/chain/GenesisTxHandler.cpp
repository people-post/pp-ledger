#include "GenesisTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "TxTyped.h"
#include "Types.h"

namespace pp {

chain_tx::Roe<uint64_t>
GenesisTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                     uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe = chain_tx::expectTx<Ledger::TxGenesis>(tx, "getSignerAccountId",
                                                    "TxGenesis");
  if (!pRoe) {
    return pRoe.error();
  }
  (void)pRoe.value();
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<bool>
GenesisTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                        uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxGenesis>(tx, "matchesWalletForIndex",
                                                    "TxGenesis");
  if (!pRoe) {
    return pRoe.error();
  }
  (void)pRoe.value();
  return walletId == AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<void> GenesisTxHandler::applyBuffer(const Ledger::TypedTx &tx,
                                                  AccountBuffer & /*bank*/,
                                                  const BufferApplyContext &c) const {
  (void)tx;
  (void)c;
  return chain_tx::TxError(
      chain_err::E_TX_TYPE,
      "Genesis transaction not allowed in buffer mode");
}

chain_tx::Roe<void> GenesisTxHandler::applyBlock(const Ledger::TypedTx &tx,
                                                 AccountBuffer & /*bank*/,
                                                 const BlockApplyContext &c) const {
  auto pRoe =
      chain_tx::expectTx<Ledger::TxGenesis>(tx, "applyBlock", "TxGenesis");
  if (!pRoe) {
    return pRoe.error();
  }

  // The genesis init transaction lives in the genesis block (blockId=0) and
  // seeds chain config + consensus + genesis account.
  if (c.blockId != 0 || c.slotLeaderId != 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE,
                             "Genesis transaction type not allowed in normal block");
  }

  return applyGenesisInit(*pRoe.value(), c.ctx);
}

chain_tx::Roe<void> GenesisTxHandler::applyGenesisInit(
    const Ledger::TxGenesis &tx, TxContext &ctx) const {
  log().info << "Processing system initialization transaction";
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

  auto attachmentRoe = chain_tx::validateAndCanonicalizeAttachment(
      gm.genesis.meta,
      [&](uint64_t id) {
        return id == AccountBuffer::ID_GENESIS || ctx.bank.hasAccount(id);
      },
      "Genesis account attachment: ");
  if (!attachmentRoe) {
    return attachmentRoe.error();
  }
  gm.genesis.meta = attachmentRoe.value();

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

std::optional<std::string>
GenesisTxHandler::getGenesisAccountMetaForTx(const Ledger::TypedTx &tx,
                                             const Ledger::Block &block) const {
  const auto *p = std::get_if<Ledger::TxGenesis>(&tx);
  if (!p) {
    return std::nullopt;
  }
  if (block.index != 0) {
    return std::nullopt;
  }
  return p->meta;
}

} // namespace pp
