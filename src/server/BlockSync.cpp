#include "BlockSync.h"
#include "../chain/BlockValidation.h"

#include <algorithm>

namespace pp {

BlockSync::BlockSync(Client &client, Hooks hooks) : BlockSync(client, std::move(hooks), Options{}) {}

BlockSync::BlockSync(Client &client, Hooks hooks, Options options)
    : client_(client), hooks_(std::move(hooks)), options_(options) {}

template <typename T, typename Fn> Client::Done<T> BlockSync::onServerThread(Fn fn) {
  std::weak_ptr<int> alive = alive_;
  const uint64_t attempt = attempt_;
  return [this, alive, attempt, fn = std::move(fn)](Client::Roe<T> result) {
    if (alive.expired()) {
      return;
    }
    hooks_.postToServerThread([this, alive, attempt, fn, result = std::move(result)]() mutable {
      if (alive.expired() || !inFlight_ || attempt != attempt_) {
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
  ++attempt_;
  phase_ = Phase::Status;
  outstanding_ = 0;
  blocksAdded_ = 0;
  fetched_.clear();
  held_.clear();
  authHashes_.clear();

  if (auto dial = hooks_.dialUpstream(); !dial) {
    finish(false, "Failed to dial upstream: " + dial.error().message);
    return Start::Started;
  }
  client_.fetchBeaconStateAsync(onServerThread<Client::BeaconState>(
      [this](Client::Roe<Client::BeaconState> result) { onStatus(std::move(result)); }));
  return Start::Started;
}

void BlockSync::onStatus(Client::Roe<Client::BeaconState> result) {
  if (!result) {
    finish(false, "Failed to get upstream status: " + result.error().message);
    return;
  }
  status_ = std::move(result.value());
  if (status_.certificate && hooks_.offerCertificate) {
    hooks_.offerCertificate(*status_.certificate);
  }
  if (status_.equivocation && hooks_.offerEquivocation) {
    hooks_.offerEquivocation(*status_.equivocation);
  }
  syncToSignedTip();
  advance();
}

void BlockSync::syncToSignedTip() {
  if (!status_.signedTip) {
    finish(false, "Upstream has no beacon signed tip");
    return;
  }
  tip_ = *status_.signedTip;
  const uint64_t next = hooks_.nextBlockId();
  const auto localTip = hooks_.tipHash();
  freshStart_ = !localTip.has_value();
  if (freshStart_) {
    // No blocks yet: the authority comes from the range's first block (authenticateFirst).
    if (tip_.height < next) {
      finish(false, "Upstream's signed tip (block " + std::to_string(tip_.height) + ") is below block " +
                        std::to_string(next) + ", where this node starts");
      return;
    }
    trail_.reset();
    linkHash_.clear();
    authBase_ = next;
    authHashes_.clear();
    held_.clear();
    const bool pinned = hooks_.firstBlockHash && hooks_.firstBlockHash().has_value();
    log().info << "No local blocks: authenticating blocks " << next << " to " << tip_.height << " from block "
               << next << (pinned ? " (pinned)" : "");
    beginRange(Phase::Authenticate, next, tip_.height + 1);
    return;
  }
  auto trail = hooks_.authorityTrail();
  if (!trail) {
    finish(false, "Beacon authority unavailable: " + trail.error().message);
    return;
  }
  trail_ = std::move(trail.value());
  // A tip of a later generation can verify only once the blocks certifying it are followed.
  if (tip_.generation <= trail_->beacon().generation && !verifyTip()) {
    return;
  }
  if (tip_.height + 1 == next && tip_.blockHash != *localTip) {
    finish(false, "Local block " + std::to_string(tip_.height) + " is not the one the beacon signed");
    return;
  }
  if (tip_.height < next) {
    if (tip_.generation > trail_->beacon().generation) {
      finish(false, "Signed tip of a later generation for a block this node already holds");
      return;
    }
    finish(true, {}); // nothing signed beyond what we hold
    return;
  }
  linkHash_ = *localTip;
  authBase_ = next;
  authHashes_.clear();
  held_.clear();
  log().info << "Syncing blocks " << next << " to " << tip_.height << " (signed tip, beacon generation "
             << tip_.generation << ")";
  beginRange(Phase::Authenticate, next, tip_.height + 1);
}

void BlockSync::beginRange(Phase phase, uint64_t from, uint64_t to) {
  phase_ = phase;
  base_ = from;
  target_ = to;
  nextToFetch_ = from;
  nextToProcess_ = from;
  fetched_.clear();
}

void BlockSync::fetchMore() {
  while (outstanding_ < options_.maxInFlightFetches && nextToFetch_ < target_) {
    const uint64_t blockId = nextToFetch_++;
    if (phase_ == Phase::Apply && (held_.count(blockId) > 0 || blockId < hooks_.nextBlockId())) {
      continue; // kept from Authenticate, or already applied another way
    }
    ++outstanding_;
    client_.fetchBlockAsync(blockId, onServerThread<Ledger::ChainNode>(
                                         [this, blockId](Client::Roe<Ledger::ChainNode> result) {
                                           onBlock(blockId, std::move(result));
                                         }));
  }
}

void BlockSync::onBlock(uint64_t blockId, Client::Roe<Ledger::ChainNode> result) {
  --outstanding_;
  if (!result) {
    finish(false, "Failed to fetch block " + std::to_string(blockId) + ": " + result.error().message);
    return;
  }
  fetched_.emplace(blockId, std::move(result.value()));
  advance();
}

void BlockSync::advance() {
  const uint64_t attempt = attempt_;
  auto running = [&]() { return inFlight_ && attempt == attempt_ && phase_ != Phase::Status; };
  while (running()) {
    if (nextToProcess_ >= target_) {
      endPhase();
      continue;
    }
    Ledger::ChainNode block;
    if (auto kept = held_.find(nextToProcess_); phase_ == Phase::Apply && kept != held_.end()) {
      block = std::move(kept->second);
      held_.erase(kept);
    } else if (auto arrived = fetched_.find(nextToProcess_); arrived != fetched_.end()) {
      block = std::move(arrived->second);
      fetched_.erase(arrived);
    } else if (phase_ == Phase::Apply && nextToProcess_ < hooks_.nextBlockId()) {
      ++nextToProcess_; // applied another way (e.g. our own block)
      continue;
    } else {
      break; // waiting for it
    }
    if (!process(std::move(block))) {
      return;
    }
    ++nextToProcess_;
  }
  if (running()) {
    fetchMore();
  }
}

bool BlockSync::process(Ledger::ChainNode block) {
  if (block.block.index != nextToProcess_) {
    finish(false, "Upstream sent block " + std::to_string(block.block.index) + " for " +
                      std::to_string(nextToProcess_));
    return false;
  }
  switch (phase_) {
  case Phase::Authenticate:
    return authenticate(std::move(block));
  case Phase::Apply:
    return apply(std::move(block));
  case Phase::Status:
    break;
  }
  return true;
}

bool BlockSync::authenticateFirst(Ledger::ChainNode &block) {
  const std::string hash = chain_block::calculateBlockHash(block.block);
  if (const auto pinned = hooks_.firstBlockHash ? hooks_.firstBlockHash() : std::nullopt;
      pinned && *pinned != hash) {
    finish(false, "Block " + std::to_string(nextToProcess_) + " is not the pinned first block");
    return false;
  }
  auto trail = AuthorityTrail::fromStartingBlock(block.block);
  if (!trail) {
    finish(false, "Cannot start from block " + std::to_string(nextToProcess_) + ": " + trail.error().message);
    return false;
  }
  if (hooks_.checkStartingAuthority) {
    if (auto vetted = hooks_.checkStartingAuthority(nextToProcess_, trail.value().authority(), trail.value().admin());
        !vetted) {
      finish(false, "Block " + std::to_string(nextToProcess_) + " carries an authority this node does not accept: " +
                        vetted.error().message);
      return false;
    }
  }
  trail_ = std::move(trail.value());
  linkHash_ = block.block.previousHash; // nothing below it to link to
  return true;
}

bool BlockSync::authenticate(Ledger::ChainNode block) {
  if (freshStart_ && nextToProcess_ == authBase_ && !authenticateFirst(block)) {
    return false;
  }
  if (block.block.previousHash != linkHash_) {
    finish(false, "Block " + std::to_string(nextToProcess_) + " does not extend the block below it");
    return false;
  }
  if (auto observed = trail_->observe(block.block); !observed) {
    finish(false, "Beacon authority does not follow: " + observed.error().message);
    return false;
  }
  block.hash = chain_block::calculateBlockHash(block.block);
  linkHash_ = block.hash;
  authHashes_.push_back(block.hash);
  if (held_.size() < options_.maxHeldBlocks) {
    held_.emplace(nextToProcess_, std::move(block));
  }
  return true;
}

bool BlockSync::apply(Ledger::ChainNode block) {
  if (nextToProcess_ < hooks_.nextBlockId()) {
    return true; // applied another way meanwhile
  }
  block.hash = chain_block::calculateBlockHash(block.block);
  if (block.hash != authHashes_.at(nextToProcess_ - authBase_)) {
    finish(false, "Block " + std::to_string(nextToProcess_) + " differs from the one authenticated");
    return false;
  }
  if (auto applied = hooks_.applyBlock(block); !applied) {
    finish(false, "Failed to add block " + std::to_string(nextToProcess_) + ": " + applied.error().message);
    return false;
  }
  ++blocksAdded_;
  return true;
}

void BlockSync::endPhase() {
  switch (phase_) {
  case Phase::Authenticate:
    if (linkHash_ != tip_.blockHash) {
      finish(false, "Blocks up to " + std::to_string(tip_.height) + " do not lead to the signed tip");
      return;
    }
    if (!verifyTip()) {
      return;
    }
    beginRange(Phase::Apply, authBase_, target_);
    return;
  case Phase::Apply:
    finish(true, {});
    return;
  case Phase::Status:
    return;
  }
}

bool BlockSync::verifyTip() {
  if (auto verified = trail_->verifyTip(tip_); !verified) {
    finish(false, "Upstream's signed tip (block " + std::to_string(tip_.height) +
                      ") does not verify: " + verified.error().message);
    return false;
  }
  return true;
}

void BlockSync::finish(bool ok, std::string error) {
  inFlight_ = false;
  ++attempt_; // late results of this sync are ignored
  phase_ = Phase::Status;
  fetched_.clear();
  held_.clear();
  authHashes_.clear();
  outstanding_ = 0;
  const bool tipVerified = ok && trail_.has_value() && status_.signedTip.has_value();
  trail_.reset();
  if (tipVerified && hooks_.onTipVerified) {
    hooks_.onTipVerified(tip_);
  }
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
  freshStart_ = false;
  if (hooks_.onFinished) {
    hooks_.onFinished(Result{ok, blocksAdded_, std::move(error)});
  }
}

} // namespace pp
