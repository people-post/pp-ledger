#ifndef PP_LEDGER_BROADCAST_TALLY_H
#define PP_LEDGER_BROADCAST_TALLY_H

#include <cstddef>

namespace pp {

/**
 * Outcome bookkeeping for one block sent to several upstreams in parallel:
 * commit on the first success; give up only when every upstream failed.
 */
class BroadcastTally {
public:
  enum class Event { None, Commit, AllFailed };

  explicit BroadcastTally(size_t upstreams) : outstanding_(upstreams) {}

  /** Record one upstream's result; returns what the caller should do now. */
  Event onResult(bool ok) {
    if (outstanding_ > 0) {
      --outstanding_;
    }
    if (ok && !committed_) {
      committed_ = true;
      return Event::Commit;
    }
    if (outstanding_ == 0 && !committed_) {
      return Event::AllFailed;
    }
    return Event::None;
  }

  bool done() const { return outstanding_ == 0; }
  bool committed() const { return committed_; }

private:
  size_t outstanding_;
  bool committed_{false};
};

} // namespace pp

#endif // PP_LEDGER_BROADCAST_TALLY_H
