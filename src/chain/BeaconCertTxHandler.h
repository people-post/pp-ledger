#ifndef PP_LEDGER_BEACON_CERT_TX_HANDLER_H
#define PP_LEDGER_BEACON_CERT_TX_HANDLER_H

#include "ITxHandler.h"

namespace pp {

/**
 * T_BEACON_CERT (beacon-authority ba2): the admin set (genesis account, M-of-N) certifies the next
 * beacon generation. Applied at once: the certificate in force changes within the block. A block
 * holds at most one.
 */
class BeaconCertTxHandler final : public ITxHandler {
public:
  /** Draft tag: one certificate per block (AccountBuffer::addDraftTag). */
  static constexpr const char *kDraftTag = "beacon-cert";

  chain_tx::Roe<uint64_t>
  getSignerAccountId(const Ledger::TypedTx &tx, uint64_t slotLeaderId) const override;

  chain_tx::Roe<bool>
  matchesWalletForIndex(const Ledger::TypedTx &tx, uint64_t walletId) const override;

  chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
  getIdempotencyKey(const Ledger::TypedTx &tx) const override;

  chain_tx::Roe<void>
  applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
              const BufferApplyContext &c) const override;

  chain_tx::Roe<void>
  applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
             const BlockApplyContext &c) const override;

  /** The certificate a T_BEACON_CERT carries (fee must be 0). */
  static chain_tx::Roe<BeaconCertificate> decodeCertificate(const Ledger::TxBeaconCert &tx);

private:
  /** Checks shared by draft and block apply; returns the certificate to put in force. */
  chain_tx::Roe<BeaconCertificate> check(const Ledger::TxBeaconCert &tx, const TxContext &ctx,
                                         AccountBuffer &bank, uint64_t blockId, uint64_t slot,
                                         chain_block::BlockAdmissionMode admissionMode) const;
};

} // namespace pp

#endif
