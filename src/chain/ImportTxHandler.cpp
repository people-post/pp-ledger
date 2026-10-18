#include "ImportTxHandler.h"
#include "AccountBuffer.h"
#include "AccountIds.h"
#include "ErrorCodes.h"
#include "TxTyped.h"
#include "lib/common/BinaryPack.hpp"

#include <algorithm>

namespace pp {

chain_tx::Roe<uint64_t> ImportTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                           uint64_t /*slotLeaderId*/) const {
  // Never consulted: an import is unsigned (Chain::validateTxSignatures).
  auto pRoe = chain_tx::expectTx<Ledger::TxImport>(tx, "getSignerAccountId", "TxImport");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId;
}

chain_tx::Roe<bool> ImportTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxImport>(tx, "matchesWalletForIndex", "TxImport");
  if (!pRoe) {
    return pRoe.error();
  }
  return pRoe.value()->walletId == walletId;
}

chain_tx::Roe<void> ImportTxHandler::applyBuffer(const Ledger::TypedTx & /*tx*/, AccountBuffer & /*bank*/,
                                                 const BufferApplyContext & /*c*/) const {
  return chain_tx::TxError(chain_err::E_TX_VALIDATION, "An import is written by the importing genesis, never pooled");
}

chain_tx::Roe<StateSnapshot::Account> ImportTxHandler::decodeAccount(const Ledger::TxImport &tx) {
  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "An import has fee 0");
  }
  auto account = utl::binaryUnpack<StateSnapshot::Account>(tx.meta);
  if (!account) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE, "Invalid imported account: " + account.error().message);
  }
  if (account.value().id != tx.walletId) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "An import's account is not its walletId");
  }
  return account.value();
}

Ledger::Record ImportTxHandler::makeRecord(const StateSnapshot::Account &account) {
  Ledger::TxImport tx;
  tx.walletId = account.id;
  tx.meta = utl::binaryPack(account);
  Ledger::Record record;
  record.type = Ledger::T_IMPORT;
  record.data = utl::binaryPack(tx);
  return record;
}

chain_tx::Roe<AccountBuffer::Account> ImportTxHandler::entering(const StateSnapshot::Account &old,
                                                                const ImportProgress &progress, uint64_t blockId) {
  AccountBuffer::Account account;
  account.id = old.id;
  account.wallet = old.wallet;
  account.blockId = blockId;
  account.attachmentHash = AccountBuffer::attachmentHashOf(old.attachment);
  // Slots and epochs of the old network mean nothing under the new genesis time.
  account.wallet.miner.lastProducedSlot = 0;
  account.wallet.miner.missedDue = 0;
  account.wallet.miner.autoDeregisteredEpoch = Client::MinerState::kNever;
  if (AccountIds::isSystemAccount(old.id)) {
    const auto keys = std::find_if(progress.systemKeys.begin(), progress.systemKeys.end(),
                                   [&](const SystemAccountKeys &k) { return k.id == old.id; });
    if (keys == progress.systemKeys.end()) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "No keys for imported system account " + std::to_string(old.id));
    }
    account.wallet.publicKeys = keys->publicKeys;
    account.wallet.minSignatures = keys->minSignatures;
  }
  return account;
}

chain_tx::Roe<void> ImportTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                const BlockApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxImport>(tx, "applyBlock", "TxImport");
  if (!pRoe) {
    return pRoe.error();
  }
  auto &progress = c.ctx.importProgress;
  if (!progress.has_value() || c.blockId == 0 || c.slotLeaderId != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "An import outside an importing genesis's span");
  }
  auto old = decodeAccount(*pRoe.value());
  if (!old) {
    return old.error();
  }
  const auto &account = old.value();
  if (progress->complete()) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "More imports than the genesis announced");
  }
  if (progress->lastId.has_value() && account.id <= *progress->lastId) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Imports go by ascending account id: " + std::to_string(account.id) + " after " +
                                 std::to_string(*progress->lastId));
  }

  // The leaf as the old chain hashed it: the predecessor's state root checks them all.
  AccountBuffer::Account oldLeaf;
  oldLeaf.id = account.id;
  oldLeaf.wallet = account.wallet;
  oldLeaf.blockId = account.blockId;
  oldLeaf.attachmentHash = AccountBuffer::attachmentHashOf(account.attachment);
  AccountBuffer::setLeaves(progress->oldLeaves, oldLeaf);

  if (account.id == AccountBuffer::ID_GENESIS) {
    // The genesis record set its keys and attachment at block 0: only the balances come over.
    auto held = bank.getAccount(AccountBuffer::ID_GENESIS);
    if (!held) {
      return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "No genesis account to import into");
    }
    AccountBuffer::Account genesis = held.value();
    genesis.wallet.mBalances = account.wallet.mBalances;
    bank.remove(AccountBuffer::ID_GENESIS);
    if (auto added = bank.add(genesis); !added) {
      return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, "Failed to import the genesis account: " +
                                                                 added.error().message);
    }
  } else {
    auto entered = entering(account, *progress, c.blockId);
    if (!entered) {
      return entered.error();
    }
    if (bank.hasAccount(account.id)) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Account " + std::to_string(account.id) + " imported twice");
    }
    if (auto added = bank.add(entered.value()); !added) {
      return chain_tx::TxError(chain_err::E_INTERNAL_BUFFER, "Failed to import account " +
                                                                 std::to_string(account.id) + ": " +
                                                                 added.error().message);
    }
  }
  ++progress->imported;
  progress->lastId = account.id;
  return {};
}

std::optional<std::string> ImportTxHandler::getUserAccountMetaForTx(const Ledger::TypedTx &tx,
                                                                   uint64_t accountId) const {
  const auto *p = std::get_if<Ledger::TxImport>(&tx);
  if (!p || accountId == AccountBuffer::ID_GENESIS || p->walletId != accountId) {
    return std::nullopt;
  }
  auto account = utl::binaryUnpack<StateSnapshot::Account>(p->meta);
  if (!account) {
    return std::nullopt;
  }
  Client::UserAccount user;
  user.wallet = account.value().wallet;
  user.meta = account.value().attachment;
  return user.ltsToString();
}

} // namespace pp
