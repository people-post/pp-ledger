#pragma once
// Domains (projects/domains; WIRE_SCHEMA § Domains): a system or issued account's symbol — its
// token's ticker and its domain. `A`–`Z`; 1–3 letters for a system account, 4–7 for an issued one;
// none for a user. Each domain has a leaf of its own in the state tree, at a key no account has.

#include "ledger/chain/TxError.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pp::chain_domain {

inline constexpr size_t kMaxLetters = 7;
inline constexpr size_t kMaxSystemLetters = 3;

/** `A`–`Z`, 1 to kMaxLetters of them. */
bool isWellFormed(std::string_view domain);

/** `text` in its stored form (upper case), if it can be a domain at all. */
std::optional<std::string> normalize(std::string_view text);

/**
 * `domain` is one account `accountId` may have: well formed, 1–3 letters for a system account, 4–7
 * for an issued one; a user account has none.
 */
chain_tx::Roe<void> validateFor(std::string_view domain, uint64_t accountId);

/** The domain's key in the state tree: 2⁶³ + its letters as base-27 digits (A = 1 … Z = 26). */
uint64_t treeKey(std::string_view domain);

/** The domain's leaf: it is bound to `accountId`. */
std::string leafHash(std::string_view domain, uint64_t accountId);

} // namespace pp::chain_domain
