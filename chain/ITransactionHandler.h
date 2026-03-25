#ifndef PP_LEDGER_I_TRANSACTION_HANDLER_H
#define PP_LEDGER_I_TRANSACTION_HANDLER_H

namespace pp {

/**
 * Per-transaction-type handler (validation + application). Phase 1: registry
 * slots are reserved; Phase 2 implements concrete handlers.
 */
class ITransactionHandler {
public:
  virtual ~ITransactionHandler() = default;
};

} // namespace pp

#endif
