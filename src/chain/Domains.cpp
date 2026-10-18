#include "Domains.h"

#include "ErrorCodes.h"
#include "../client/AccountIds.h"
#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"

#include <cctype>
#include <sstream>

namespace pp::chain_domain {

bool isWellFormed(std::string_view domain) {
  if (domain.empty() || domain.size() > kMaxLetters) {
    return false;
  }
  for (char c : domain) {
    if (c < 'A' || c > 'Z') {
      return false;
    }
  }
  return true;
}

std::optional<std::string> normalize(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (!isWellFormed(out)) {
    return std::nullopt;
  }
  return out;
}

chain_tx::Roe<void> validateFor(std::string_view domain, uint64_t accountId) {
  const std::string shown(domain);
  if (!isWellFormed(domain)) {
    return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                             "A domain is 1 to 7 capital letters A-Z: '" + shown + "'");
  }
  if (AccountIds::isSystemAccount(accountId)) {
    if (domain.size() > kMaxSystemLetters) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "A system account's domain is 1 to 3 letters: '" + shown + "'");
    }
    return {};
  }
  if (AccountIds::isIssuedAccount(accountId)) {
    if (domain.size() <= kMaxSystemLetters) {
      return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                               "An issued account's domain is 4 to 7 letters (1 to 3 are the system's): '" +
                                   shown + "'");
    }
    return {};
  }
  return chain_tx::TxError(chain_err::E_TX_VALIDATION,
                           "A user account has no domain (account " + std::to_string(accountId) + ")");
}

uint64_t treeKey(std::string_view domain) {
  uint64_t code = 0;
  for (char c : domain) {
    code = code * 27 + static_cast<uint64_t>(c - 'A' + 1);
  }
  return AccountIds::ID_LIMIT + code;
}

std::string leafHash(std::string_view domain, uint64_t accountId, bool retired) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  std::string name(domain);
  ar & name & accountId & retired;
  return utl::sha256Raw(std::string("pp-ledger/domain-leaf/v1") + oss.str());
}

} // namespace pp::chain_domain
