#include "TxSignatures.h"
#include "ErrorCodes.h"
#include "lib/common/Utilities.h"

#include <algorithm>

namespace pp::chain_tx {

Roe<void> verifySignaturesAgainstAccount(
    const std::string &message, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account, const Crypto &crypto,
    logging::Logger &logger) {
  if (signatures.size() < account.wallet.minSignatures) {
    return TxError(
        chain_err::E_TX_SIGNATURE,
        "Account " + std::to_string(account.id) + " must have at least " +
            std::to_string(int(account.wallet.minSignatures)) +
            " signatures, but has " + std::to_string(signatures.size()));
  }
  if (account.wallet.publicKeys.size() > kMaxSignatureCount) {
    return TxError(chain_err::E_TX_SIGNATURE,
                   "Account " + std::to_string(account.id) +
                       " has more public keys than allowed (" +
                       std::to_string(account.wallet.publicKeys.size()) +
                       " > " + std::to_string(kMaxSignatureCount) + ")");
  }
  // No valid multisig ever needs more signatures than the account has keys;
  // reject early instead of burning a verify() call per extra signature.
  if (signatures.size() > account.wallet.publicKeys.size() ||
      signatures.size() > kMaxSignatureCount) {
    return TxError(chain_err::E_TX_SIGNATURE,
                   "Account " + std::to_string(account.id) +
                       " received more signatures (" +
                       std::to_string(signatures.size()) +
                       ") than allowed (" +
                       std::to_string(std::min(account.wallet.publicKeys.size(),
                                               kMaxSignatureCount)) +
                       ")");
  }
  std::vector<bool> keyUsed(account.wallet.publicKeys.size(), false);
  for (const auto &signature : signatures) {
    bool matched = false;
    for (size_t i = 0; i < account.wallet.publicKeys.size(); ++i) {
      if (keyUsed[i])
        continue;
      const auto &publicKey = account.wallet.publicKeys[i];
      if (crypto.verify(account.wallet.keyType, publicKey, message,
                        signature)) {
        keyUsed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      // Summary only: dumping every public key and signature on each
      // failure is log spam and needlessly puts raw key/signature bytes in
      // the log for every rejected transaction.
      logger.error << "Invalid or duplicate signature for account "
                   << account.id << " (minSignatures="
                   << int(account.wallet.minSignatures)
                   << ", publicKeys=" << account.wallet.publicKeys.size()
                   << ", signaturesProvided=" << signatures.size() << ")";
      return TxError(chain_err::E_TX_SIGNATURE,
                     "Invalid or duplicate signature for account " +
                         std::to_string(account.id));
    }
  }
  return {};
}

} // namespace pp::chain_tx
