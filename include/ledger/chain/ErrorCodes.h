#ifndef PP_LEDGER_CHAIN_ERROR_CODES_H
#define PP_LEDGER_CHAIN_ERROR_CODES_H

#include <cstdint>

namespace pp::chain_err {

// Mirrors Chain operational codes (single source for shared tx helpers).

constexpr int32_t E_STATE_INIT = 1;
constexpr int32_t E_STATE_MOUNT = 2;
constexpr int32_t E_INVALID_ARGUMENT = 3;

constexpr int32_t E_BLOCK_NOT_FOUND = 10;
constexpr int32_t E_BLOCK_SEQUENCE = 11;
constexpr int32_t E_BLOCK_HASH = 12;
constexpr int32_t E_BLOCK_INDEX = 13;
constexpr int32_t E_BLOCK_CHAIN = 14;
constexpr int32_t E_BLOCK_VALIDATION = 15;
constexpr int32_t E_BLOCK_GENESIS = 16;

constexpr int32_t E_CONSENSUS_SLOT_LEADER = 30;
constexpr int32_t E_CONSENSUS_TIMING = 31;
constexpr int32_t E_CONSENSUS_QUERY = 32;

constexpr int32_t E_ACCOUNT_NOT_FOUND = 40;
constexpr int32_t E_ACCOUNT_EXISTS = 41;
constexpr int32_t E_ACCOUNT_BALANCE = 42;
constexpr int32_t E_ACCOUNT_BUFFER = 43;
constexpr int32_t E_ACCOUNT_RENEWAL = 44;

constexpr int32_t E_TX_VALIDATION = 60;
constexpr int32_t E_TX_SIGNATURE = 61;
constexpr int32_t E_TX_FEE = 62;
constexpr int32_t E_TX_AMOUNT = 63;
constexpr int32_t E_TX_TYPE = 64;
constexpr int32_t E_TX_TRANSFER = 65;
constexpr int32_t E_TX_IDEMPOTENCY = 66;
constexpr int32_t E_TX_VALIDATION_TIMESPAN = 67;
constexpr int32_t E_TX_TIME_OUTSIDE_WINDOW = 68;

constexpr int32_t E_LEDGER_WRITE = 80;
constexpr int32_t E_LEDGER_READ = 81;

constexpr int32_t E_INTERNAL_DESERIALIZE = 90;
constexpr int32_t E_INTERNAL_BUFFER = 91;
constexpr int32_t E_INTERNAL = 99;

} // namespace pp::chain_err

#endif
