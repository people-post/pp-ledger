#include "chain/BeaconCertTxHandler.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountPolicy.h"
#include "ledger/chain/ErrorCodes.h"
#include "chain/TxTyped.h"
#include "ledger/chain/Types.h"

#include <variant>

namespace pp {

chain_tx::Roe<uint64_t> BeaconCertTxHandler::getSignerAccountId(const Ledger::TypedTx &tx,
                                                                uint64_t slotLeaderId) const {
  (void)slotLeaderId;
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconCert>(tx, "getSignerAccountId", "TxBeaconCert");
  if (!pRoe) {
    return pRoe.error();
  }
  return AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<bool> BeaconCertTxHandler::matchesWalletForIndex(const Ledger::TypedTx &tx,
                                                               uint64_t walletId) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconCert>(tx, "matchesWalletForIndex", "TxBeaconCert");
  if (!pRoe) {
    return pRoe.error();
  }
  return walletId == AccountBuffer::ID_GENESIS;
}

chain_tx::Roe<std::optional<std::pair<uint64_t, uint64_t>>>
BeaconCertTxHandler::getIdempotencyKey(const Ledger::TypedTx &tx) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconCert>(tx, "getIdempotencyKey", "TxBeaconCert");
  if (!pRoe) {
    return pRoe.error();
  }
  const auto *p = pRoe.value();
  if (p->idempotentId == 0) {
    return std::optional<std::pair<uint64_t, uint64_t>>{};
  }
  return std::optional<std::pair<uint64_t, uint64_t>>(std::make_pair(AccountBuffer::ID_GENESIS, p->idempotentId));
}

chain_tx::Roe<BeaconCertificate> BeaconCertTxHandler::decodeCertificate(const Ledger::TxBeaconCert &tx) {
  if (tx.fee != 0) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Beacon certificate transaction must have fee 0");
  }
  BeaconCertificate cert;
  if (!cert.ltsFromString(tx.meta)) {
    return chain_tx::TxError(chain_err::E_INTERNAL_DESERIALIZE, "Failed to deserialize beacon certificate");
  }
  return cert;
}

chain_tx::Roe<BeaconCertificate> BeaconCertTxHandler::check(const Ledger::TxBeaconCert &tx, const TxContext &ctx,
                                                            AccountBuffer &bank, uint64_t blockId, uint64_t slot,
                                                            chain_block::BlockAdmissionMode admissionMode) const {
  if (blockId == 0) {
    return chain_tx::TxError(chain_err::E_TX_TYPE, "The genesis record certifies generation 1");
  }
  auto cert = decodeCertificate(tx);
  if (!cert) {
    return cert.error();
  }
  // A certificate with a cutover commits only in block C+1, after the new beacon takes over — hours
  // or days after the admins signed it; it binds nodes out of band meanwhile, where no window
  // applies. Its block is pinned by cutoverHash and a generation commits once, so it needs neither
  // a validity window nor an idempotency key (bp3).
  if (!cert.value().hasCutover()) {
    if (auto idem = validateIdempotencyUsingContext(ctx, tx.idempotentId, AccountBuffer::ID_GENESIS,
                                                    tx.validationTsMin, tx.validationTsMax, slot, admissionMode);
        !idem) {
      return idem.error();
    }
  }
  if (!ctx.optAuthority.has_value() || !ctx.optChainConfig.has_value()) {
    return chain_tx::TxError(chain_err::E_STATE_INIT, "Chain authority not initialized");
  }
  if (auto frozen = chain_tx::requireAdminSetNotFrozen(ctx.optAuthority.value(), "Beacon certificate",
                                                       ctx.consensus.getEpochFromSlot(slot));
      !frozen) {
    return frozen.error();
  }
  if (auto seeded = chain_tx::seedCommittedAccount(bank, ctx.bank, AccountBuffer::ID_GENESIS); !seeded) {
    return seeded.error();
  }
  auto admin = bank.getAccount(AccountBuffer::ID_GENESIS);
  if (!admin) {
    return chain_tx::TxError(chain_err::E_ACCOUNT_NOT_FOUND, "Genesis account not found");
  }
  if (auto valid = chain_tx::validateNextBeaconCertificate(cert.value(), ctx.optAuthority.value(),
                                                          admin.value().wallet, ctx.optChainConfig->networkId,
                                                          blockId);
      !valid) {
    return valid.error();
  }
  // Block C is the one before this (validateNextBeaconCertificate); a node warming up from this very
  // block does not hold it, and takes the block on its signed tip.
  if (cert.value().hasCutover() && blockId - 1 >= ctx.ledger.getStartingBlockId() &&
      blockId - 1 < ctx.ledger.getNextBlockId()) {
    auto cutoverBlock = ctx.ledger.readBlock(blockId - 1);
    if (!cutoverBlock) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Cannot read the cutover block: " +
                                                               cutoverBlock.error().message);
    }
    if (auto same = chain_tx::validateCutoverBlock(cert.value(), cutoverBlock.value().hash); !same) {
      return same.error();
    }
  }
  return cert;
}

bool BeaconCertTxHandler::commitsCutover(const Ledger::Block &block) {
  for (const auto &record : block.records) {
    if (record.type != Ledger::T_BEACON_CERT) {
      continue;
    }
    auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record.data);
    if (!tx) {
      continue;
    }
    if (auto cert = decodeCertificate(tx.value()); cert && cert.value().hasCutover()) {
      return true;
    }
  }
  return false;
}

chain_tx::Roe<void> BeaconCertTxHandler::applyBuffer(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                     const BufferApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconCert>(tx, "applyBuffer", "TxBeaconCert");
  if (!pRoe) {
    return pRoe.error();
  }
  // Draft apply validates against the chain's authority, which changes only when a block applies:
  // a second certificate would pass here and fail the block. One per block, then.
  if (bank.hasDraftTag(kDraftTag)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "A block holds at most one beacon certificate");
  }
  auto cert = check(*pRoe.value(), c.ctx, bank, c.blockId, c.effectiveSlot, c.admissionMode);
  if (!cert) {
    return cert.error();
  }
  bank.addDraftTag(kDraftTag);
  return {};
}

chain_tx::Roe<void> BeaconCertTxHandler::applyBlock(const Ledger::TypedTx &tx, AccountBuffer &bank,
                                                    const BlockApplyContext &c) const {
  auto pRoe = chain_tx::expectTx<Ledger::TxBeaconCert>(tx, "applyBlock", "TxBeaconCert");
  if (!pRoe) {
    return pRoe.error();
  }
  auto cert = check(*pRoe.value(), c.ctx, bank, c.blockId, c.blockSlot, c.admissionMode);
  if (!cert) {
    return cert.error();
  }
  c.ctx.optAuthority->beacon = chain_tx::certificateInForce(cert.value(), c.blockId);
  log().info << "Beacon certificate generation " << cert.value().generation << " in force from block "
             << c.blockId << " (the previous generation signed up to " << c.blockId - 1 << ")";
  return {};
}

} // namespace pp
