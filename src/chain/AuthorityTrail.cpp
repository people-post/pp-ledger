#include "AuthorityTrail.h"
#include "AccountPolicy.h"
#include "BeaconCertTxHandler.h"
#include "ErrorCodes.h"
#include "TxSignatures.h"
#include "lib/common/BinaryPack.hpp"

namespace pp {

AuthorityTrail::AuthorityTrail(std::string networkId, ChainAuthority authority, Client::Wallet admin)
    : networkId_(std::move(networkId)), authority_(std::move(authority)), admin_(std::move(admin)),
      logger_(logging::getLogger("AuthorityTrail")) {}

chain_tx::Roe<void> AuthorityTrail::observe(const Ledger::Block &block) {
  for (const auto &record : block.records) {
    chain_tx::Roe<void> observed;
    if (record.type == Ledger::T_CONFIG) {
      observed = observeConfig(record);
    } else if (record.type == Ledger::T_BEACON_CERT) {
      observed = observeCertificate(record, block.index);
    }
    if (!observed) {
      return chain_tx::TxError(observed.error().code, "Block " + std::to_string(block.index) + ": " +
                                                          observed.error().message);
    }
  }
  return {};
}

chain_tx::Roe<void> AuthorityTrail::verifyAdminSigned(const Ledger::Record &record) const {
  AccountBuffer::Account admin;
  admin.id = AccountBuffer::ID_GENESIS;
  admin.wallet = admin_;
  return chain_tx::verifySignaturesAgainstAccount(record.signingMessage(networkId_), record.signatures, admin,
                                                  crypto_, logger_);
}

chain_tx::Roe<void> AuthorityTrail::observeConfig(const Ledger::Record &record) {
  // A config update replaces the admin set at once; only the admin set in force may sign it.
  auto tx = utl::binaryUnpack<Ledger::TxConfig>(record.data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Unreadable config update");
  }
  if (auto signed_ = verifyAdminSigned(record); !signed_) {
    return chain_tx::TxError(chain_err::E_TX_SIGNATURE,
                             "Config update not signed by the admin set: " + signed_.error().message);
  }
  admin_ = gm.genesis.wallet;
  return {};
}

chain_tx::Roe<void> AuthorityTrail::observeCertificate(const Ledger::Record &record, uint64_t blockId) {
  auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record.data);
  if (!tx) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Unreadable beacon certificate");
  }
  auto cert = BeaconCertTxHandler::decodeCertificate(tx.value());
  if (!cert) {
    return cert.error();
  }
  if (auto signed_ = verifyAdminSigned(record); !signed_) {
    return chain_tx::TxError(chain_err::E_TX_SIGNATURE,
                             "Beacon certificate not signed by the admin set: " + signed_.error().message);
  }
  if (auto valid = chain_tx::validateNextBeaconCertificate(cert.value(), authority_, admin_, networkId_, blockId);
      !valid) {
    return valid.error();
  }
  authority_.beacon = cert.value();
  return {};
}

std::optional<ChainAuthority> AuthorityTrail::carriedBy(const Ledger::Block &block) {
  for (const auto &record : block.records) {
    std::string meta;
    if (record.type == Ledger::T_GENESIS) {
      auto tx = utl::binaryUnpack<Ledger::TxGenesis>(record.data);
      if (!tx) {
        return std::nullopt;
      }
      meta = tx.value().meta;
    } else if (record.type == Ledger::T_RENEWAL) {
      auto tx = utl::binaryUnpack<Ledger::TxRenewal>(record.data);
      if (!tx || tx.value().walletId != AccountBuffer::ID_GENESIS) {
        continue;
      }
      meta = tx.value().meta;
    } else {
      continue;
    }
    GenesisAccountMeta gm;
    if (!gm.ltsFromString(meta)) {
      return std::nullopt;
    }
    return gm.authority;
  }
  return std::nullopt;
}

chain_tx::Roe<void> AuthorityTrail::verifyTip(const Client::SignedTip &tip) const {
  const auto &beacon = authority_.beacon;
  if (tip.networkId != networkId_) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Signed tip is for network '" + tip.networkId + "'");
  }
  if (tip.generation < beacon.generation) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Signed tip is from beacon generation " + std::to_string(tip.generation) +
                                 ", superseded by generation " + std::to_string(beacon.generation) +
                                 " (cutover at block " + std::to_string(beacon.cutoverHeight) + ")");
  }
  if (tip.generation > beacon.generation) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Signed tip is from beacon generation " + std::to_string(tip.generation) +
                                 ", which no certificate here names (generation " +
                                 std::to_string(beacon.generation) + " is in force)");
  }
  if (!tip.verify(beacon.publicKey)) {
    return chain_tx::TxError(chain_err::E_TX_SIGNATURE, "Signed tip is not signed by the beacon of generation " +
                                                            std::to_string(beacon.generation));
  }
  return {};
}

} // namespace pp
