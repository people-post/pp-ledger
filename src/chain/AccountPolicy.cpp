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
    uint64_t selfAccountId, std::string_view deserializeErrorMsg) {
  Client::UserAccount userAccount;
  if (!userAccount.ltsFromString(serializedMeta)) {
    return TxError(chain_err::E_INTERNAL_DESERIALIZE,
                   std::string(deserializeErrorMsg));
  }

  auto attachmentRoe = validateAndCanonicalizeAttachment(
      userAccount.meta,
      makeWorkingSetPublisherExists(working, committedOrNull, selfAccountId),
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
  if (beacon.cutoverHeight != 0) {
    return invalid("The genesis beacon certificate must have cutoverHeight 0");
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
  if (next.cutoverHeight < current.beacon.cutoverHeight) {
    return invalid("Beacon certificate cutoverHeight must not go back");
  }
  if (commitBlockId.has_value() && next.cutoverHeight >= *commitBlockId) {
    return invalid("Beacon certificate cutoverHeight must be below the block that commits it");
  }
  return {};
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
