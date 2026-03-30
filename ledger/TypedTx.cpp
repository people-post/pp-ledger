#include "TypedTx.h"

#include "lib/common/BinaryPack.hpp"

namespace pp {

Ledger::Roe<TypedTx> decodeRecordToTypedTx(const Ledger::Record &rec) {
  switch (rec.type) {
  case Ledger::T_DEFAULT: {
    auto txRoe = utl::binaryUnpack<Ledger::TxDefault>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxDefault payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_GENESIS: {
    auto txRoe = utl::binaryUnpack<Ledger::TxGenesis>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxGenesis payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_NEW_USER: {
    auto txRoe = utl::binaryUnpack<Ledger::TxNewUser>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxNewUser payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_CONFIG: {
    auto txRoe = utl::binaryUnpack<Ledger::TxConfig>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxConfig payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_USER_UPDATE: {
    auto txRoe = utl::binaryUnpack<Ledger::TxUserUpdate>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxUserUpdate payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_RENEWAL: {
    auto txRoe = utl::binaryUnpack<Ledger::TxRenewal>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxRenewal payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  case Ledger::T_END_USER: {
    auto txRoe = utl::binaryUnpack<Ledger::TxEndUser>(rec.data);
    if (!txRoe) {
      return Ledger::Error(1, "Invalid packed TxEndUser payload: " +
                                  txRoe.error().message);
    }
    return TypedTx(txRoe.value());
  }
  default:
    return Ledger::Error(1, "Unknown transaction type: " +
                                std::to_string(rec.type));
  }
}

} // namespace pp

