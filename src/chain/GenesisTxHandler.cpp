#include "GenesisTxHandler.h"
#include "AccountBuffer.h"
#include "AccountPolicy.h"
#include "BlockValidation.h"
#include "ErrorCodes.h"
#include "TxTyped.h"
#include "Types.h"
#include "AccountIds.h"
#include "lib/common/Utilities.h"

#include <set>

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

chain_tx::Roe<void> GenesisTxHandler::openImportSpan(const GenesisAccountMeta &gm, TxContext &ctx) const {
  const auto &import = gm.import;
  const auto refused = [](const std::string &why) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Importing genesis: " + why);
  };
  if (import.predecessor.networkId.empty() || import.predecessor.networkId == gm.config.networkId) {
    return refused("the predecessor's network id must be set and differ from this network's");
  }
  if (import.predecessor.blockHash.size() != utl::SHA256_DIGEST_SIZE ||
      import.predecessor.stateRoot.size() != utl::SHA256_DIGEST_SIZE) {
    return refused("the predecessor's block hash and state root must be 32 bytes");
  }
  if (auto bound = chain_block::validatePredecessorHeader(import.predecessor); !bound) {
    return refused(bound.error().message);
  }
  if (import.accountCount == 0) {
    return refused("no accounts to import");
  }
  if (!gm.genesis.wallet.mBalances.empty()) {
    return refused("the genesis account's balances come with the import");
  }
  // Reserve, registrar, fee and recycle, once each, with keys; genesis's are the meta's own.
  std::set<uint64_t> ids;
  for (const auto &keys : import.systemKeys) {
    if (!AccountIds::isSystemAccount(keys.id) || keys.id == AccountBuffer::ID_GENESIS || keys.id > AccountIds::ID_RECYCLE ||
        !ids.insert(keys.id).second) {
      return refused("system account keys for id " + std::to_string(keys.id));
    }
    if (keys.publicKeys.empty() || keys.minSignatures == 0 || keys.minSignatures > keys.publicKeys.size()) {
      return refused("system account " + std::to_string(keys.id) + " needs keys and a threshold they can meet");
    }
  }
  if (ids.size() != AccountIds::ID_RECYCLE) {
    return refused("keys for every system account (reserve, registrar, fee, recycle)");
  }
  ImportProgress progress;
  progress.predecessor = import.predecessor;
  progress.accountCount = import.accountCount;
  progress.systemKeys = import.systemKeys;
  ctx.importProgress = std::move(progress);
  log().info << "Importing " << import.accountCount << " accounts of network '" << import.predecessor.networkId
             << "' at block " << import.predecessor.height;
  return {};
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

  if (gm.hasPending) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Genesis config cannot carry a pending update");
  }
  if (auto authority = chain_tx::validateGenesisAuthority(gm.authority, gm.genesis.wallet, gm.config.networkId);
      !authority) {
    return authority.error();
  }
  ctx.optChainConfig = gm.config;
  ctx.optAuthority = gm.authority;

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
  genesisAccount.attachmentHash = AccountBuffer::attachmentHashOf(gm.genesis.meta);
  auto roeAddGenesis = ctx.bank.add(genesisAccount);
  if (!roeAddGenesis) {
    return chain_tx::TxError(
        chain_err::E_INTERNAL_BUFFER,
        "Failed to add genesis account to buffer: " +
            roeAddGenesis.error().message);
  }

  if (gm.hasImport) {
    if (auto opened = openImportSpan(gm, ctx); !opened) {
      return opened;
    }
  }

  log().info << "System initialized";
  log().info << "  Version: " << gm.VERSION;
  log().info << "  Config: " << ctx.optChainConfig.value();
  log().info << "  Authority: " << gm.authority;
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
