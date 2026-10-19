#ifndef PP_LEDGER_TX_TYPED_H
#define PP_LEDGER_TX_TYPED_H

#include "ErrorCodes.h"
#include "TxError.h"
#include "../ledger/Ledger.h"

#include <string>
#include <variant>

namespace pp::chain_tx {

/**
 * Safe TypedTx downcast used by handlers. Returns a pointer into `tx` on
 * success so callers keep the same `*p` usage style as `std::get_if`.
 */
template <typename T>
Roe<const T *> expectTx(const Ledger::TypedTx &tx, const char *where,
                        const char *typeName) {
  const auto *p = std::get_if<T>(&tx);
  if (!p) {
    return TxError(chain_err::E_INTERNAL,
                   std::string(where) + ": expected " + typeName);
  }
  return p;
}

} // namespace pp::chain_tx

#endif
