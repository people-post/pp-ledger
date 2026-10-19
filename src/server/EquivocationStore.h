#ifndef PP_LEDGER_EQUIVOCATION_STORE_H
#define PP_LEDGER_EQUIVOCATION_STORE_H

#include "chain/Chain.h"

#include "ledger/client/Client.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"

#include <optional>
#include <string>

namespace pp {

/**
 * Equivocation evidence a node holds (beacon-authority ba6): found in its own tips, served on
 * STATUS, or pushed up by a downstream (EQUIVOCATION_ADD). The chain verifies and keeps it
 * (Chain::offerEquivocation) and halts the generation above its height; this store writes it to
 * `equivocation.dat` beside the ledger before the offer returns, so a restart stays halted. The
 * file is removed once a later generation supersedes it.
 *
 * A node with no blocks cannot verify evidence yet: the stored one waits for the first block
 * (retryPending). Server thread only.
 */
class EquivocationStore : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  static constexpr const int32_t E_IO = 1;

  EquivocationStore(Chain &chain, std::string path);

  /** After mount: offer the stored evidence to the chain again (removed if superseded). */
  Roe<void> load();

  /** Offer `evidence` to the chain; persist it when kept. */
  Roe<Chain::EquivocationOffer> offer(const Client::Equivocation &evidence);

  /** offer() evidence a peer serves or pushes (`from`): a refusal is logged once. None when refused. */
  std::optional<Chain::EquivocationOffer> note(const Client::Equivocation &evidence, const std::string &from);

  /** Stored evidence the chain could not take yet (no blocks): offer it again. */
  void retryPending();

  /** After the authority moved on (a later certificate): remove the file if the chain resumed. */
  void refresh();

private:
  void offerStored(const Client::Equivocation &evidence);
  void remove();

  Chain &chain_;
  std::string path_;
  bool stored_{false};
  std::optional<Client::Equivocation> pending_;
  /** Bytes of the last evidence note() reported as refused. */
  std::string lastRefused_;
};

} // namespace pp

#endif // PP_LEDGER_EQUIVOCATION_STORE_H
