#include "AccountPolicy.h"
#include "ErrorCodes.h"
#include "../client/AccountAttachment.h"

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
