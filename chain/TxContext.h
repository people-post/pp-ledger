#ifndef PP_LEDGER_TX_CONTEXT_H
#define PP_LEDGER_TX_CONTEXT_H

#include "Types.h"
#include "AccountBuffer.h"
#include "TxFees.h"
#include "TxIdempotency.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"
#include "lib/common/Crypto.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pp {

/** Callables for scanning block records for serialized account metadata. */
struct FnAccountMetaForRecord {
  std::function<std::optional<std::string>(const Ledger::Record &, uint64_t)>
      fnUser;
  std::function<std::optional<std::string>(const Ledger::Record &,
                                             const Ledger::Block &)>
      fnGenesis;
};

/**
 * Chain subsystem bundle passed to transaction handlers.
 * Chain owns a single TxContext member; handlers take TxContext & or
 * const TxContext & depending on whether they may mutate chain state.
 */
struct TxContext {
  Crypto crypto;
  consensus::Ouroboros consensus;
  Ledger ledger;
  AccountBuffer bank;
  std::optional<BlockChainConfig> optChainConfig{std::nullopt};
  Checkpoint checkpoint{};
  std::optional<FnAccountMetaForRecord> fnAccountMetaForRecord{std::nullopt};
  std::optional<chain_tx::FnIdempotencyKeyForRecord> fnIdempotencyKeyForRecord{
      std::nullopt};
  std::optional<chain_tx::FnBillableCustomMetaSizeForFee>
      fnBillableCustomMetaSizeForFee{std::nullopt};
};

/** Scratch-buffer / mempool path after signature validation. */
struct BufferApplyContext {
  const TxContext &ctx;
  uint64_t blockId{0};
  uint64_t effectiveSlot{0};
  bool isStrictMode{true};
};

/** Committed-chain (block replay) path after signature validation. */
struct BlockApplyContext {
  TxContext &ctx;
  uint64_t blockId{0};
  uint64_t blockSlot{0};
  uint64_t slotLeaderId{0};
  bool isStrictMode{true};
};

} // namespace pp

#endif
