#include "AuthorityTrail.h"
#include "AccountPolicy.h"
#include "BlockValidation.h"
#include "BeaconCertTxHandler.h"
#include "ErrorCodes.h"
#include "TxSignatures.h"
#include "lib/common/BinaryPack.hpp"

namespace pp {

AuthorityTrail::AuthorityTrail(std::string networkId, ChainAuthority authority, Client::Wallet admin)
    : networkId_(std::move(networkId)), authority_(std::move(authority)), admin_(std::move(admin)),
      logger_(logging::getLogger("AuthorityTrail")) {}

chain_tx::Roe<AuthorityTrail> AuthorityTrail::fromGenesis(const Ledger::Block &genesis) {
  if (genesis.index != 0 || genesis.records.empty() || genesis.records[0].type != Ledger::T_GENESIS) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Not a genesis block");
  }
  auto tx = utl::binaryUnpack<Ledger::TxGenesis>(genesis.records[0].data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return chain_tx::TxError(chain_err::E_BLOCK_GENESIS, "Unreadable genesis meta");
  }
  return AuthorityTrail(gm.config.networkId, gm.authority, gm.genesis.wallet);
}

chain_tx::Roe<AuthorityTrail> AuthorityTrail::fromStartingBlock(const Ledger::Block &block) {
  if (block.index == 0) {
    return fromGenesis(block);
  }
  if (block.records.empty() || !chain_block::isGenesisRenewal(block.records.front())) {
    return chain_tx::TxError(chain_err::E_BLOCK_VALIDATION,
                             "Block " + std::to_string(block.index) +
                                 " is not a checkpoint: it does not open with the genesis renewal");
  }
  auto tx = utl::binaryUnpack<Ledger::TxRenewal>(block.records.front().data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Unreadable genesis renewal");
  }
  return AuthorityTrail(gm.config.networkId, gm.authority, gm.genesis.wallet);
}

bool AuthorityTrail::changesAuthority(const Ledger::Record &record) {
  return record.type == Ledger::T_CONFIG || record.type == Ledger::T_BEACON_CERT;
}

chain_tx::Roe<void> AuthorityTrail::observe(const Ledger::Block &block) {
  for (const auto &record : block.records) {
    if (auto observed = observeRecord(record, block.index); !observed) {
      return observed;
    }
  }
  return {};
}

chain_tx::Roe<void> AuthorityTrail::observeRecord(const Ledger::Record &record, uint64_t blockId) {
  chain_tx::Roe<void> observed;
  if (record.type == Ledger::T_CONFIG) {
    observed = observeConfig(record);
  } else if (record.type == Ledger::T_BEACON_CERT) {
    observed = observeCertificate(record, blockId);
  } else if (chain_block::isGenesisRenewal(record)) {
    observed = observeGenesisRenewal(record);
  }
  if (!observed) {
    return chain_tx::TxError(observed.error().code, "Block " + std::to_string(blockId) + ": " +
                                                        observed.error().message);
  }
  return {};
}

chain_tx::Roe<ChainAuthority> AuthorityTrail::replayProof(const Ledger::Block &genesis,
                                                          const Client::AuthorityProof &proof,
                                                          uint64_t upToBlockId) {
  auto trail = fromGenesis(genesis);
  if (!trail) {
    return trail.error();
  }
  uint64_t lastBlockId = 0;
  for (const auto &step : proof.steps) {
    if (step.blockId == 0 || step.blockId < lastBlockId || step.blockId > upToBlockId) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "Authority proof step at block " + std::to_string(step.blockId) +
                                   " is out of order or beyond block " + std::to_string(upToBlockId));
    }
    if (!changesAuthority(step.record)) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Authority proof carries an unrelated record");
    }
    if (auto observed = trail.value().observeRecord(step.record, step.blockId); !observed) {
      return observed.error();
    }
    lastBlockId = step.blockId;
  }
  return trail.value().authority();
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

chain_tx::Roe<void> AuthorityTrail::observeGenesisRenewal(const Ledger::Record &record) {
  // A renewal re-records the genesis meta in force: it changes nothing, so it must agree.
  auto tx = utl::binaryUnpack<Ledger::TxRenewal>(record.data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION, "Unreadable genesis renewal");
  }
  const auto &wallet = gm.genesis.wallet;
  if (gm.authority != authority_ || wallet.publicKeys != admin_.publicKeys ||
      wallet.minSignatures != admin_.minSignatures || wallet.keyType != admin_.keyType) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "Genesis renewal disagrees with the authority and admin set followed");
  }
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
