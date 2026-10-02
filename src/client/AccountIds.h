#ifndef PP_LEDGER_ACCOUNT_IDS_H
#define PP_LEDGER_ACCOUNT_IDS_H

#include <cstdint>

namespace pp {

/**
 * Well-known ledger account ids (shared by client envelope validation and
 * AccountBuffer — keep out of chain↔client cycles).
 */
struct AccountIds {
  constexpr static uint64_t ID_GENESIS = 0;
  constexpr static uint64_t ID_FEE = 1;
  constexpr static uint64_t ID_RESERVE = 2;
  constexpr static uint64_t ID_RECYCLE = 3;

  /**
   * Account id ranges:
   *   [0, ID_FIRST_ISSUED)              system accounts — protocol-owned, created only in
   *                                     the genesis block; never terminated, renew fee-free
   *   [ID_FIRST_ISSUED, ID_FIRST_USER)  issued accounts — created by the genesis wallet
   *                                     after genesis (e.g. token issuers, genesis miners)
   *   [ID_FIRST_USER, ...)              user accounts
   */
  constexpr static uint64_t ID_FIRST_ISSUED = 1ULL << 20;

  /** User account ids are >= this value (~1e9). */
  constexpr static uint64_t ID_FIRST_USER = 1ULL << 30;

  constexpr static bool isSystemAccount(uint64_t id) { return id < ID_FIRST_ISSUED; }

  constexpr static uint64_t INITIAL_TOKEN_SUPPLY = 1ULL << 30;
};

} // namespace pp

#endif
