#include "BlockSync.h"

#include <algorithm>

namespace pp {

BlockSync::BlockSync(Client &client, Hooks hooks) : BlockSync(client, std::move(hooks), Options{}) {}

BlockSync::BlockSync(Client &client, Hooks hooks, Options options)
    : client_(client), hooks_(std::move(hooks)), options_(options) {}

template <typename T, typename Fn> Client::Done<T> BlockSync::onServerThread(Fn fn) {
  std::weak_ptr<int> alive = alive_;
  const uint64_t generation = generation_;
  return [this, alive, generation, fn = std::move(fn)](Client::Roe<T> result) {
    if (alive.expired()) {
      return;
    }
    hooks_.postToServerThread([this, alive, generation, fn, result = std::move(result)]() mutable {
      if (alive.expired() || !inFlight_ || generation != generation_) {
        return; // destroyed, or a result of a sync that already finished
      }
      fn(std::move(result));
    });
  };
}

BlockSync::Start BlockSync::start() {
  if (inFlight_) {
    return Start::AlreadyRunning;
  }
  if (std::chrono::steady_clock::now() < notBefore_) {
    return Start::BackingOff;
  }
  inFlight_ = true;
  ++generation_;
  target_ = 0;
  nextToFetch_ = 0;
  outstanding_ = 0;
  blocksAdded_ = 0;
  fetched_.clear();

  if (auto dial = hooks_.dialUpstream(); !dial) {
    finish(false, "Failed to dial upstream: " + dial.error().message);
    return Start::Started;
  }
  const uint64_t generation = generation_;
  client_.fetchCalibrationAsync(onServerThread<Client::CalibrationResponse>(
      [this, generation](Client::Roe<Client::CalibrationResponse> result) {
        onCalibrated(generation, std::move(result));
      }));
  return Start::Started;
}

void BlockSync::onCalibrated(uint64_t /*generation*/, Client::Roe<Client::CalibrationResponse> result) {
  if (!result) {
    finish(false, "Failed to get upstream calibration: " + result.error().message);
    return;
  }
  target_ = result.value().nextBlockId;
  nextToFetch_ = hooks_.nextBlockId();
  if (nextToFetch_ >= target_) {
    finish(true, {});
    return;
  }
  log().info << "Syncing blocks " << nextToFetch_ << " to " << target_;
  fetchMore();
}

void BlockSync::fetchMore() {
  while (outstanding_ < options_.maxInFlightFetches && nextToFetch_ < target_) {
    const uint64_t blockId = nextToFetch_++;
    ++outstanding_;
    const uint64_t generation = generation_;
    client_.fetchBlockAsync(blockId, onServerThread<Ledger::ChainNode>(
                                         [this, generation, blockId](Client::Roe<Ledger::ChainNode> result) {
                                           onBlock(generation, blockId, std::move(result));
                                         }));
  }
}

void BlockSync::onBlock(uint64_t /*generation*/, uint64_t blockId, Client::Roe<Ledger::ChainNode> result) {
  --outstanding_;
  if (!result) {
    finish(false, "Failed to fetch block " + std::to_string(blockId) + ": " + result.error().message);
    return;
  }
  fetched_.emplace(blockId, std::move(result.value()));
  if (!applyReady()) {
    return;
  }
  if (hooks_.nextBlockId() >= target_) {
    finish(true, {});
    return;
  }
  // The tip may have moved past blocks still to fetch (applied another way).
  nextToFetch_ = std::max(nextToFetch_, hooks_.nextBlockId());
  fetchMore();
}

bool BlockSync::applyReady() {
  uint64_t expected = hooks_.nextBlockId();
  fetched_.erase(fetched_.begin(), fetched_.lower_bound(expected)); // already have these
  for (auto it = fetched_.find(expected); it != fetched_.end(); it = fetched_.find(expected)) {
    auto applied = hooks_.applyBlock(it->second);
    if (!applied) {
      finish(false, "Failed to add block " + std::to_string(expected) + ": " + applied.error().message);
      return false;
    }
    ++blocksAdded_;
    fetched_.erase(it);
    expected = hooks_.nextBlockId();
  }
  return true;
}

void BlockSync::finish(bool ok, std::string error) {
  inFlight_ = false;
  ++generation_; // late results of this sync are ignored
  fetched_.clear();
  outstanding_ = 0;
  if (ok) {
    backoff_ = std::chrono::milliseconds(0);
    notBefore_ = {};
    if (blocksAdded_ > 0) {
      log().info << "Sync complete: " << blocksAdded_ << " blocks added";
    }
  } else {
    backoff_ = backoff_.count() == 0 ? options_.initialBackoff : std::min(backoff_ * 2, options_.maxBackoff);
    notBefore_ = std::chrono::steady_clock::now() + backoff_;
    log().warning << "Block sync failed (retry in " << backoff_.count() << " ms): " << error;
  }
  if (hooks_.onFinished) {
    hooks_.onFinished(Result{ok, blocksAdded_, std::move(error)});
  }
}

} // namespace pp
