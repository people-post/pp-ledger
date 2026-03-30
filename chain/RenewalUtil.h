#ifndef PP_LEDGER_RENEWAL_UTIL_H
#define PP_LEDGER_RENEWAL_UTIL_H

#include "../ledger/Ledger.h"

namespace pp {

/** Map miner-signed user renewal payload to user-update upsert semantics. */
inline Ledger::TxUserUpdate
renewalToUserUpsert(const Ledger::TxRenewal &tx) {
  Ledger::TxUserUpdate userTx;
  userTx.walletId = tx.walletId;
  userTx.fee = tx.fee;
  userTx.meta = tx.meta;
  userTx.idempotentId = 0;
  userTx.validationTsMin = 0;
  userTx.validationTsMax = 0;
  return userTx;
}

} // namespace pp

#endif
