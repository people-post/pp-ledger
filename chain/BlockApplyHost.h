#ifndef PP_LEDGER_BLOCK_APPLY_HOST_H
#define PP_LEDGER_BLOCK_APPLY_HOST_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "../ledger/Ledger.h"

#include <cstdint>

namespace pp {

class ITxHandler;
struct TxContext;

/** Hooks for applying decoded txs to committed chain state (block replay). */
class IBlockApplyHost {
public:
  virtual ~IBlockApplyHost() = default;

  virtual chain_tx::Roe<void>
  validateIdempotency(const Ledger::TxDefault &tx, uint64_t effectiveSlot,
                      bool isStrictMode) const = 0;
  virtual chain_tx::Roe<void>
  validateIdempotency(const Ledger::TxNewUser &tx, uint64_t effectiveSlot,
                      bool isStrictMode) const = 0;
  virtual chain_tx::Roe<void>
  validateIdempotency(const Ledger::TxConfig &tx, uint64_t effectiveSlot,
                      bool isStrictMode) const = 0;
  virtual chain_tx::Roe<void>
  validateIdempotency(const Ledger::TxUserUpdate &tx, uint64_t effectiveSlot,
                      bool isStrictMode) const = 0;
};

struct BlockApplyContext {
  const IBlockApplyHost &host;
  TxContext &ctx;
  ITxHandler *userUpdateHandler{ nullptr };
  uint64_t blockId{ 0 };
  uint64_t blockSlot{ 0 };
  uint64_t slotLeaderId{ 0 };
  bool isStrictMode{ true };
};

} // namespace pp

#endif

