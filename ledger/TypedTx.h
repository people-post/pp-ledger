#pragma once

#include "Ledger.h"

#include <variant>

namespace pp {

/**
 * In-memory typed transaction payload.
 *
 * On-ledger representation remains Ledger::Record{type,data,signatures} where
 * data is binaryPack(TxX). This variant is a decoded view for centralized
 * dispatch.
 */
using TypedTx = std::variant<
    Ledger::TxDefault,
    Ledger::TxGenesis,
    Ledger::TxNewUser,
    Ledger::TxConfig,
    Ledger::TxUserUpdate,
    Ledger::TxRenewal,
    Ledger::TxEndUser>;

/** Decode Ledger::Record into a typed tx payload. */
Ledger::Roe<TypedTx> decodeRecordToTypedTx(const Ledger::Record &rec);

} // namespace pp

