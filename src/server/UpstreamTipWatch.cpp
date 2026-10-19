#include "server/UpstreamTipWatch.h"

#include <algorithm>

namespace pp {

UpstreamTipWatch::UpstreamTipWatch(Client &client, Hooks hooks)
    : UpstreamTipWatch(client, std::move(hooks), Options{}) {}

UpstreamTipWatch::UpstreamTipWatch(Client &client, Hooks hooks, Options options)
    : client_(client), hooks_(std::move(hooks)), options_(options) {}

void UpstreamTipWatch::maintain() {
  if (inFlight_ || std::chrono::steady_clock::now() < notBefore_) {
    return;
  }
  if (auto dial = hooks_.dialUpstream(); !dial) {
    onReply(Client::Roe<Client::TipReply>(Client::Error(Client::E_NOT_CONNECTED, dial.error().message)));
    return;
  }
  inFlight_ = true;
  std::weak_ptr<int> alive = alive_;
  const uint64_t known = std::max(hooks_.nextBlockId(), upstreamNext_);
  client_.waitForBlockAsync(known, [this, alive](Client::Roe<Client::TipReply> result) {
    if (alive.expired()) {
      return;
    }
    hooks_.postToServerThread([this, alive, result = std::move(result)]() mutable {
      if (!alive.expired()) {
        onReply(std::move(result));
      }
    });
  });
}

void UpstreamTipWatch::onReply(Client::Roe<Client::TipReply> result) {
  inFlight_ = false;
  if (!result) {
    backoff_ = backoff_.count() == 0 ? options_.initialBackoff : std::min(backoff_ * 2, options_.maxBackoff);
    notBefore_ = std::chrono::steady_clock::now() + backoff_;
    log().debug << "Block wait failed (retry in " << backoff_.count() << " ms): " << result.error().message;
    return;
  }
  backoff_ = std::chrono::milliseconds(0);
  notBefore_ = {};
  upstreamNext_ = result.value().nextBlockId; // may drop after a failover to a shorter upstream
  if (result.value().hasSignedTip && hooks_.onSignedTip) {
    hooks_.onSignedTip(result.value().signedTip);
  }
  if (upstreamNext_ > hooks_.nextBlockId()) {
    hooks_.onUpstreamAhead();
  }
}

} // namespace pp
