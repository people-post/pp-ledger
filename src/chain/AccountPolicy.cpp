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

Roe<void> replaceGenesisAccount(AccountBuffer &bank, uint64_t blockId,
                                const Client::Wallet &wallet) {
  bank.remove(AccountBuffer::ID_GENESIS);

  AccountBuffer::Account account;
  account.id = AccountBuffer::ID_GENESIS;
  account.blockId = blockId;
  account.wallet = wallet;
  auto addResult = bank.add(account);
  if (!addResult) {
    return TxError(chain_err::E_INTERNAL_BUFFER,
                   "Failed to add updated genesis account: " +
                       addResult.error().message);
  }
  return {};
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
