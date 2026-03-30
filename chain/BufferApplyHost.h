#ifndef PP_LEDGER_BUFFER_APPLY_HOST_H
#define PP_LEDGER_BUFFER_APPLY_HOST_H

#include "AccountBuffer.h"
#include "TxContext.h"
#include "TxError.h"
#include "../ledger/Ledger.h"

#include <cstdint>

namespace pp {

class ITxHandler;

/** Hooks for buffer-mode tx application that require chain state (idempotency,
 * seeding accounts into scratch buffer). Implemented by Chain. */
class IBufferApplyHost {
public:
  virtual ~IBufferApplyHost() = default;

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

  /** Same semantics as Chain::ensureAccountInBuffer (scratch buffer seeding). */
  virtual chain_tx::Roe<void>
  seedAccountIntoBuffer(AccountBuffer &bank, uint64_t accountId) const = 0;
};

struct BufferApplyContext {
  const IBufferApplyHost &host;
  const TxContext &ctx;
  ITxHandler *userUpdateHandler{ nullptr };
  uint64_t blockId{ 0 };
  uint64_t effectiveSlot{ 0 };
  bool isStrictMode{ true };
};

} // namespace pp

#endif
