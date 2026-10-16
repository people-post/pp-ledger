#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "../client/AccountAttachment.h"
#include "lib/common/Utilities.h"

#include <algorithm>
#include <set>

namespace pp::chain_tx {

Roe<void> validateUserWalletBasics(const Crypto &crypto,
                                   const Client::Wallet &wallet) {
  if (!crypto.isSupported(wallet.keyType)) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Unsupported key type: " +
                       std::to_string(int(wallet.keyType)));
  }
  if (wallet.publicKeys.empty()) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "User account must have at least one public key");
  }
  if (wallet.minSignatures < 1) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "User account must require at least one signature");
  }
  return {};
}

Roe<std::string>
validateAndCanonicalizeAttachment(const std::string &raw,
                                  const FnPublisherExists &publisherExists,
                                  std::string_view errorPrefix) {
  auto parsed = AccountAttachment::parseAndValidate(raw, publisherExists);
  if (!parsed) {
    return TxError(chain_err::E_TX_VALIDATION,
                   std::string(errorPrefix) + parsed.error().message);
  }
  return parsed->ltsToString();
}

FnPublisherExists makeWorkingSetPublisherExists(
    const AccountBuffer &working, const AccountBuffer *committedOrNull,
    uint64_t selfAccountId) {
  return [&working, committedOrNull, selfAccountId](uint64_t id) {
    if (id == selfAccountId) {
      return true;
    }
    if (working.hasAccount(id)) {
      return true;
    }
    return committedOrNull != nullptr && committedOrNull->hasAccount(id);
  };
}

Roe<Client::UserAccount> loadAndValidateUserAccountMeta(
    const std::string &serializedMeta, const Crypto &crypto,
    const AccountBuffer &working, const AccountBuffer *committedOrNull,
    uint64_t selfAccountId, std::string_view deserializeErrorMsg, bool requireKnownPublishers) {
  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(serializedMeta)) {
    return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                   std::string(deserializeErrorMsg));
  }

  const FnPublisherExists anyPublisher = [](uint64_t) { return true; };
  auto attachmentRoe = validateAndCanonicalizeAttachment(
      userAccount.meta,
      requireKnownPublishers ? makeWorkingSetPublisherExists(working, committedOrNull, selfAccountId) : anyPublisher,
      "Account attachment: ");
  if (!attachmentRoe) {
    return attachmentRoe.error();
  }
  userAccount.meta = attachmentRoe.value();

  if (auto walletOk = validateUserWalletBasics(crypto, userAccount.wallet);
      !walletOk) {
    return walletOk.error();
  }
  return userAccount;
}

Roe<void> validateGenesisWalletShape(const Client::Wallet &wallet) {
  if (wallet.miner != Client::MinerState{}) {
    return TxError(chain_err::E_TX_VALIDATION, "The genesis account cannot be a miner");
  }
  if (wallet.publicKeys.size() < 3) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Genesis account must have at least 3 public keys");
  }
  if (wallet.minSignatures < 2) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Genesis account must have at least 2 signatures");
  }
  return {};
}

Roe<void> validateGenesisAuthority(const ChainAuthority &authority, const Client::Wallet &adminWallet,
                                   const std::string &networkId) {
  auto invalid = [](const std::string &what) { return TxError(chain_err::E_TX_VALIDATION, what); };
  auto isAdminKey = [&](const std::string &key) {
    return std::find(adminWallet.publicKeys.begin(), adminWallet.publicKeys.end(), key) !=
           adminWallet.publicKeys.end();
  };
  if (authority.hasPendingRecovery) {
    return invalid("Genesis cannot carry a pending recovery");
  }
  const RecoverySet &recovery = authority.recovery;
  if (recovery.publicKeys.empty()) {
    return invalid("Recovery set needs at least one public key");
  }
  std::set<std::string> seen;
  for (const auto &key : recovery.publicKeys) {
    if (key.size() != utl::kMlDsaPublicKeyBytes) {
      return invalid("Recovery set has a malformed public key");
    }
    if (!seen.insert(key).second) {
      return invalid("Recovery set lists the same key twice");
    }
    if (isAdminKey(key)) {
      return invalid("A recovery key must not be an admin (genesis account) key");
    }
  }
  if (recovery.minSignatures == 0 || recovery.minSignatures > recovery.publicKeys.size()) {
    return invalid("Recovery minSignatures must be between 1 and its key count");
  }
  if (recovery.delayEpochs == 0) {
    return invalid("Recovery delayEpochs must be at least 1");
  }
  const BeaconCertificate &beacon = authority.beacon;
  if (beacon.generation != 1) {
    return invalid("The genesis beacon certificate must be generation 1");
  }
  if (beacon.cutoverHeight != 0 || !beacon.cutoverHash.empty()) {
    return invalid("The genesis beacon certificate must have cutoverHeight 0 and no cutoverHash");
  }
  if (auto hints = validateCertificateHints(beacon); !hints) {
    return hints;
  }
  if (beacon.networkId != networkId) {
    return invalid("Beacon certificate networkId does not match the chain");
  }
  if (beacon.publicKey.size() != utl::kMlDsaPublicKeyBytes) {
    return invalid("Beacon certificate has a malformed public key");
  }
  if (isAdminKey(beacon.publicKey) || seen.count(beacon.publicKey) != 0) {
    return invalid("The beacon signing key must not be an admin or recovery key");
  }
  return {};
}

Roe<void> validateNextBeaconCertificate(const BeaconCertificate &next, const ChainAuthority &current,
                                        const Client::Wallet &adminWallet, const std::string &networkId,
                                        std::optional<uint64_t> commitBlockId) {
  auto invalid = [](const std::string &what) { return TxError(chain_err::E_TX_VALIDATION, what); };
  if (next.networkId != networkId) {
    return invalid("Beacon certificate networkId does not match the chain");
  }
  if (commitBlockId.has_value() ? next.generation != current.beacon.generation + 1
                                : next.generation <= current.beacon.generation) {
    return invalid("Beacon certificate must be generation " +
                   std::string(commitBlockId.has_value() ? "" : "above ") +
                   std::to_string(current.beacon.generation + (commitBlockId.has_value() ? 1 : 0)));
  }
  if (next.publicKey.size() != utl::kMlDsaPublicKeyBytes) {
    return invalid("Beacon certificate has a malformed public key");
  }
  const auto &recoveryKeys = current.recovery.publicKeys;
  if (next.publicKey == current.beacon.publicKey ||
      std::find(adminWallet.publicKeys.begin(), adminWallet.publicKeys.end(), next.publicKey) !=
          adminWallet.publicKeys.end() ||
      std::find(recoveryKeys.begin(), recoveryKeys.end(), next.publicKey) != recoveryKeys.end()) {
    return invalid("The new beacon key must not be the current beacon key, an admin key or a recovery key");
  }
  if (auto shape = validateCertificateHints(next); !shape) {
    return shape;
  }
  if (!(next.hasCutover() ? next.cutoverHash.size() == utl::SHA256_DIGEST_SIZE : next.cutoverHash.empty())) {
    return invalid("Beacon certificate cutoverHash must be the 32-byte hash of the cutover block, and "
                   "empty with no cutover");
  }
  if (next.cutoverHeight < current.beacon.cutoverHeight) {
    return invalid("Beacon certificate cutoverHeight must not go back");
  }
  // The block that committed the generation in force (the one after its cutover) stays: cutting it
  // would leave a chain whose own certificate is gone (beacon-authority bp2).
  if (next.hasCutover() && current.beacon.generation > 1 && next.cutoverHeight <= current.beacon.cutoverHeight) {
    return invalid("Beacon certificate cutoverHeight " + std::to_string(next.cutoverHeight) +
                   " would drop block " + std::to_string(current.beacon.cutoverHeight + 1) +
                   ", which committed generation " + std::to_string(current.beacon.generation));
  }
  if (commitBlockId.has_value() && *commitBlockId - 1 > next.cutoverHeight) {
    return invalid("Beacon certificate cutoverHeight " + std::to_string(next.cutoverHeight) +
                   " is passed: the previous generation would have signed up to block " +
                   std::to_string(*commitBlockId - 1));
  }
  // With a cutover the admins vouch for block C's hash, so block C must exist: the certificate
  // commits right after it.
  if (commitBlockId.has_value() && next.hasCutover() && *commitBlockId != next.cutoverHeight + 1) {
    return invalid("Beacon certificate with a cutover at block " + std::to_string(next.cutoverHeight) +
                   " commits in block " + std::to_string(next.cutoverHeight + 1) + ", not " +
                   std::to_string(*commitBlockId));
  }
  return {};
}

Roe<void> validateCertificateHints(const BeaconCertificate &cert) {
  constexpr size_t kMaxHints = 8;
  constexpr size_t kMaxHintBytes = 256;
  if (cert.dialHints.size() > kMaxHints) {
    return TxError(chain_err::E_TX_VALIDATION,
                   "Beacon certificate carries more than " + std::to_string(kMaxHints) + " dial hints");
  }
  for (const auto &hint : cert.dialHints) {
    if (hint.empty() || hint.size() > kMaxHintBytes) {
      return TxError(chain_err::E_TX_VALIDATION, "Beacon certificate dial hint is empty or too long");
    }
  }
  return {};
}

Roe<void> validateCutoverBlock(const BeaconCertificate &cert, const std::string &cutoverBlockHash) {
  if (cert.hasCutover() && cert.cutoverHash != cutoverBlockHash) {
    return TxError(chain_err::E_TX_VALIDATION, "Beacon certificate cutoverHash is not the hash of block " +
                                                   std::to_string(cert.cutoverHeight));
  }
  return {};
}

BeaconCertificate certificateInForce(BeaconCertificate cert, uint64_t commitBlockId) {
  cert.cutoverHeight = commitBlockId - 1;
  return cert;
}

Roe<void> requireAdminSetNotFrozen(const ChainAuthority &authority, std::string_view what,
                                   std::optional<uint64_t> epoch) {
  if (!authority.hasPendingRecovery || (epoch && *epoch >= authority.pendingRecovery.applyEpoch)) {
    return {};
  }
  return TxError(chain_err::E_TX_VALIDATION,
                 std::string(what) + " refused: a recovery announced at block " +
                     std::to_string(authority.pendingRecovery.announcedAt) +
                     " freezes the admin set until it applies at epoch " +
                     std::to_string(authority.pendingRecovery.applyEpoch));
}

Roe<void> validateRecoveryAnnouncement(const RecoveryAnnouncement &announcement, const ChainAuthority &current,
                                       const std::string &networkId) {
  auto invalid = [](const std::string &what) { return TxError(chain_err::E_TX_VALIDATION, what); };
  if (announcement.networkId != networkId) {
    return invalid("Recovery announcement networkId does not match the chain");
  }
  if (current.hasPendingRecovery) {
    return invalid("A recovery is already pending (applies at epoch " +
                   std::to_string(current.pendingRecovery.applyEpoch) + ")");
  }
  const auto &keys = announcement.adminPublicKeys;
  if (keys.size() < 3 || keys.size() > kMaxSignatureCount) {
    return invalid("The new admin set must have between 3 and " + std::to_string(kMaxSignatureCount) + " keys");
  }
  if (announcement.adminMinSignatures < 2 || announcement.adminMinSignatures > keys.size()) {
    return invalid("The new admin set's minSignatures must be between 2 and its key count");
  }
  const auto &recoveryKeys = current.recovery.publicKeys;
  std::set<std::string> seen;
  for (const auto &key : keys) {
    if (key.size() != utl::kMlDsaPublicKeyBytes) {
      return invalid("The new admin set has a malformed public key");
    }
    if (!seen.insert(key).second) {
      return invalid("The new admin set lists the same key twice");
    }
    if (std::find(recoveryKeys.begin(), recoveryKeys.end(), key) != recoveryKeys.end() ||
        key == current.beacon.publicKey) {
      return invalid("A new admin key must not be a recovery key or the beacon key");
    }
  }
  return {};
}

Client::Wallet adminWalletAfterRecovery(Client::Wallet admin, const RecoveryAnnouncement &announcement) {
  admin.publicKeys = announcement.adminPublicKeys;
  admin.minSignatures = announcement.adminMinSignatures;
  return admin;
}

void advanceAuthorityToEpoch(ChainAuthority &authority, Client::Wallet &admin, uint64_t epoch) {
  if (!authority.hasPendingRecovery || epoch < authority.pendingRecovery.applyEpoch) {
    return;
  }
  admin = adminWalletAfterRecovery(std::move(admin), authority.pendingRecovery.announcement);
  authority.hasPendingRecovery = false;
  authority.pendingRecovery = {};
}

Roe<void> verifyRecoverySigned(const Ledger::Record &record, const RecoverySet &recovery,
                               const std::string &networkId, const Crypto &crypto, logging::Logger &logger) {
  // The recovery set is no account: check its keys the way an account's are checked.
  AccountBuffer::Account signer;
  signer.id = AccountBuffer::ID_GENESIS;
  signer.wallet.publicKeys = recovery.publicKeys;
  signer.wallet.minSignatures = recovery.minSignatures;
  signer.wallet.keyType = Crypto::TK_ML_DSA_65;
  return verifySignaturesAgainstAccount(record.signingMessage(networkId), record.signatures, signer, crypto, logger);
}

Roe<void> replaceAccount(AccountBuffer &bank, uint64_t accountId,
                         uint64_t blockId, const Client::Wallet &wallet,
                         std::string_view addFailurePrefix) {
  bank.remove(accountId);

  AccountBuffer::Account account;
  account.id = accountId;
  account.blockId = blockId;
  account.wallet = wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return TxError(chain_err::E_INTERNAL_BUFFER,
                   std::string(addFailurePrefix) + addResult.error().message);
  }
  return {};
}

Roe<void> replaceGenesisAccount(AccountBuffer &bank, uint64_t blockId,
                                const Client::Wallet &wallet) {
  return replaceAccount(bank, AccountBuffer::ID_GENESIS, blockId, wallet,
                        "Failed to add updated genesis account: ");
}

Roe<void> applyTransferToHeldAccounts(AccountBuffer &bank, uint64_t fromId, uint64_t toId, uint64_t tokenId,
                                      uint64_t amount, uint64_t fee) {
  const auto leg = [](const AccountBuffer::Roe<void> &result, const std::string &what) -> Roe<void> {
    if (!result) {
      return TxError(chain_err::E_TX_TRANSFER, what + ": " + result.error().message);
    }
    return {};
  };
  if (bank.hasAccount(fromId)) {
    if (auto paid = leg(bank.withdrawBalance(fromId, tokenId, static_cast<int64_t>(amount)), "Payer"); !paid) {
      return paid;
    }
    if (fee > 0) {
      if (auto paid = leg(bank.withdrawBalance(fromId, AccountBuffer::ID_GENESIS, static_cast<int64_t>(fee)),
                          "Payer fee");
          !paid) {
        return paid;
      }
    }
  }
  if (bank.hasAccount(toId)) {
    if (auto credited = leg(bank.depositBalance(toId, tokenId, static_cast<int64_t>(amount)), "Payee"); !credited) {
      return credited;
    }
  }
  return creditFeeToFeeAccount(bank, fee, "Fee: ");
}

Roe<void> creditFeeToFeeAccount(AccountBuffer &bank, uint64_t fee,
                                std::string_view failurePrefix) {
  if (fee == 0 || !bank.hasAccount(AccountBuffer::ID_FEE)) {
    return {};
  }
  auto depositResult = bank.depositBalance(AccountBuffer::ID_FEE,
                                           AccountBuffer::ID_GENESIS,
                                           static_cast<int64_t>(fee));
  if (!depositResult) {
    return TxError(chain_err::E_TX_TRANSFER,
                   std::string(failurePrefix) + depositResult.error().message);
  }
  return {};
}

Roe<void> mapBufferError(const AccountBuffer::Roe<void> &result) {
  if (!result) {
    return TxError(result.error().code, result.error().message);
  }
  return {};
}

Roe<void> seedCommittedAccount(AccountBuffer &working,
                               const AccountBuffer &committed,
                               uint64_t accountId) {
  return mapBufferError(
      working.seedFromCommittedIfMissing(committed, accountId));
}

Roe<void> seedFeeAccountIfNeeded(AccountBuffer &working,
                                 const AccountBuffer &committed, uint64_t fee) {
  if (fee == 0) {
    return {};
  }
  return seedCommittedAccount(working, committed, AccountBuffer::ID_FEE);
}

Roe<size_t> billableUserCustomMetaSize(const BlockChainConfig &config,
                                       const std::string &serializedUserAccount) {
  if (serializedUserAccount.size() <= config.freeCustomMetaSize) {
    return 0;
  }
  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(serializedUserAccount)) {
    return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                   "Failed to deserialize user account metadata for fee "
                   "calculation");
  }
  return userAccount.meta.size();
}

} // namespace pp::chain_tx
