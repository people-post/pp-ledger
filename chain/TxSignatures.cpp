#include "ChainTxSignatures.h"
#include "ChainErrorCodes.h"
#include "lib/common/Utilities.h"

namespace pp::chain_tx {

Roe<void> verifySignaturesAgainstAccount(
    const Ledger::Transaction &tx, const std::vector<std::string> &signatures,
    const AccountBuffer::Account &account, const Crypto &crypto,
    logging::Logger &logger) {
  if (signatures.size() < account.wallet.minSignatures) {
    return TxError(
        chain_err::E_TX_SIGNATURE,
        "Account " + std::to_string(account.id) + " must have at least " +
            std::to_string(int(account.wallet.minSignatures)) +
            " signatures, but has " + std::to_string(signatures.size()));
  }
  auto message = utl::binaryPack(tx);
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
      logger.error << "Invalid signature for account " +
                          std::to_string(account.id) + ": " +
                          utl::toJsonSafeString(signature);
      logger.error << "Expected signatures: "
                   << int(account.wallet.minSignatures);
      for (size_t i = 0; i < account.wallet.publicKeys.size(); ++i) {
        logger.error << "Public key " << i << ": "
                     << utl::toJsonSafeString(account.wallet.publicKeys[i]);
        logger.error << "Key used: " << keyUsed[i];
      }
      for (const auto &sig : signatures) {
        logger.error << "Signature: " << utl::toJsonSafeString(sig);
      }
      return TxError(chain_err::E_TX_SIGNATURE,
                     "Invalid or duplicate signature for account " +
                         std::to_string(account.id));
    }
  }
  return {};
}

} // namespace pp::chain_tx
