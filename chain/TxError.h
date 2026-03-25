#ifndef PP_LEDGER_CHAIN_TX_ERROR_H
#define PP_LEDGER_CHAIN_TX_ERROR_H

#include "lib/common/ResultOrError.hpp"

namespace pp::chain_tx {

struct TxError : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};

template <typename T> using Roe = ResultOrError<T, TxError>;

} // namespace pp::chain_tx

#endif
