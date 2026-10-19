#ifndef PP_LEDGER_UPSTREAM_SIGNED_TIP_H
#define PP_LEDGER_UPSTREAM_SIGNED_TIP_H

#include "ledger/client/Client.h"

#include <optional>

namespace pp {

/**
 * The newest beacon signed tip a node has heard from upstream (beacon-authority ba3), served on
 * to its downstreams unchanged. Keeps one: a higher height, or a higher generation at the same
 * height, replaces it. Not verified here; adoption checks come with ba4. Server thread only.
 */
class UpstreamSignedTip {
public:
  enum class Offer {
    Newer,    ///< kept
    Same,     ///< already held
    Older,    ///< ignored
    Conflict, ///< same generation and height, another block: ignored here (equivocation evidence)
  };

  Offer offer(const Client::SignedTip &tip) {
    if (!latest_ || tip.height > latest_->height ||
        (tip.height == latest_->height && tip.generation > latest_->generation)) {
      latest_ = tip;
      return Offer::Newer;
    }
    if (tip.height < latest_->height || tip.generation < latest_->generation) {
      return Offer::Older;
    }
    // One block signed twice is not a conflict: ML-DSA signatures carry fresh randomness.
    return tip.blockHash == latest_->blockHash ? Offer::Same : Offer::Conflict;
  }

  const std::optional<Client::SignedTip> &latest() const { return latest_; }
  /** Forget the held tip (it no longer verifies, e.g. past a certificate's cutover). */
  void reset() { latest_.reset(); }

private:
  std::optional<Client::SignedTip> latest_;
};

} // namespace pp

#endif // PP_LEDGER_UPSTREAM_SIGNED_TIP_H
