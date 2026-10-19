#ifndef PP_LEDGER_IMPORT_TX_HANDLER_H
#define PP_LEDGER_IMPORT_TX_HANDLER_H

#include "ledger/chain/ITxHandler.h"
#include "ledger/chain/StateSnapshot.h"

namespace pp {

/**
 * T_IMPORT (beacon-authority B032): one account of the network a re-genesis comes from, in an
 * import block after an importing genesis. Unsigned — the predecessor's state root authenticates
 * it — and only while the span is open (TxContext::importProgress), ids strictly ascending.
 *
 * Its old leaf (as the old chain hashed it) goes into the span's tree; the account enters the new
 * state at this block, with its slot-counting miner fields reset (`registered` kept). System
 * accounts take the keys the genesis sets; the genesis account only takes the imported balances
 * (its keys, attachment and block 0 stay the genesis record's).
 */
class ImportTxHandler final : public ITxHandler {
public:
  chain_tx::Roe<uint64_t> getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<bool> matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<void> applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                  const BufferApplyContext &c) const override;

  chain_tx::Roe<void> applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                 const BlockApplyContext &c) const override;

  /** The account's attachment, from the record that last wrote it (Chain::getAccount). */
  std::optional<std::string> getUserAccountMetaForTx(const Ledger::TypedTx &tx, uint64_t accountId) const override;

  /** The account a T_IMPORT carries (its id must match `walletId`, its fee 0). */
  static chain_tx::Roe<StateSnapshot::Account> decodeAccount(const Ledger::TxImport &tx);

  /** A T_IMPORT record for `account` (unsigned). */
  static Ledger::Record makeRecord(const StateSnapshot::Account &account);

private:
  /** The account as it enters the new state: block `blockId`, keys and miner fields as above. */
  static chain_tx::Roe<AccountBuffer::Account> entering(const StateSnapshot::Account &old, const ImportProgress &progress,
                                                        uint64_t blockId);
};

} // namespace pp

#endif
