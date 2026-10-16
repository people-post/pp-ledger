#include "UpstreamSet.h"

#include "amp/link/AdpMultiaddr.h"

#include <algorithm>

namespace pp {

UpstreamSet::UpstreamSet(std::vector<std::string> configured, Hooks hooks)
    : upstreams_(std::move(configured)), hooks_(std::move(hooks)) {
  redirectLogger("UpstreamSet");
}

const std::string &UpstreamSet::active() const {
  static const std::string none;
  return active_ < upstreams_.size() ? upstreams_[active_] : none;
}

void UpstreamSet::follow(size_t index, const std::string &why) {
  active_ = index;
  log().info << "Following upstream [" << index << "] " << upstreams_[index] << ": " << why;
  if (hooks_.onFollow) {
    hooks_.onFollow(upstreams_[index]);
  }
}

size_t UpstreamSet::addHints(const std::vector<std::string> &hints) {
  size_t added = 0;
  for (const auto &hint : hints) {
    if (upstreams_.size() >= kMaxUpstreams) {
      break;
    }
    if (std::find(upstreams_.begin(), upstreams_.end(), hint) != upstreams_.end()) {
      continue;
    }
    if (auto parsed = pp::amp::ParseAdpMultiaddr(hint); !parsed) {
      log().warning << "Ignoring dial hint '" << hint << "': " << parsed.error().message;
      continue;
    }
    if (hooks_.adopt && !hooks_.adopt(hint)) {
      continue;
    }
    upstreams_.push_back(hint);
    ++added;
    log().info << "Added upstream [" << upstreams_.size() - 1 << "] " << hint << " (a beacon certificate's hint)";
  }
  return added;
}

std::optional<size_t> UpstreamSet::choose(const std::vector<std::optional<Client::BeaconState>> &states,
                                          const std::function<bool(const Client::SignedTip &)> &mayFollow,
                                          const std::set<size_t> &avoid, size_t active) {
  auto eligible = [&](size_t i) {
    if (i >= states.size() || !states[i] || avoid.count(i) > 0) {
      return false;
    }
    const auto &tip = states[i]->signedTip;
    return !tip || !mayFollow || mayFollow(*tip);
  };
  auto generation = [&](size_t i) { return states[i]->signedTip ? states[i]->signedTip->generation : 0; };
  std::optional<size_t> best;
  for (size_t i = 0; i < states.size(); ++i) {
    if (!eligible(i)) {
      continue;
    }
    if (!best || generation(i) > generation(*best) ||
        (generation(i) == generation(*best) && states[i]->nextBlockId > states[*best]->nextBlockId)) {
      best = i;
    }
  }
  if (best && *best != active && eligible(active) && generation(active) == generation(*best) &&
      states[active]->nextBlockId + kLagTolerance >= states[*best]->nextBlockId) {
    return active;
  }
  return best;
}

std::set<size_t> UpstreamSet::avoided() const {
  std::set<size_t> out;
  const auto now = Clock::now();
  for (const auto &[index, until] : avoidUntil_) {
    if (now < until) {
      out.insert(index);
    }
  }
  return out;
}

const std::optional<Client::BeaconState> &UpstreamSet::lastStatus(size_t index) const {
  static const std::optional<Client::BeaconState> none;
  return index < states_.size() ? states_[index] : none;
}

void UpstreamSet::rerank(const std::string &why) {
  if (upstreams_.empty() || !hooks_.fetchStatus) {
    return;
  }
  if (inFlight_) {
    again_ = true; // after this round
    why_ = why;
    return;
  }
  inFlight_ = true;
  why_ = why;
  lastRanked_ = Clock::now();
  const uint64_t round = ++round_;
  const size_t count = upstreams_.size(); // a hint added meanwhile waits for the next round
  pending_ = count;
  states_.assign(count, std::nullopt);
  for (size_t i = 0; i < count && round == round_; ++i) {
    hooks_.fetchStatus(i, upstreams_[i], [this, round, i](Client::Roe<Client::BeaconState> status) {
      onStatus(round, i, std::move(status));
    });
  }
}

void UpstreamSet::onStatus(uint64_t round, size_t index, Client::Roe<Client::BeaconState> status) {
  if (round != round_ || !inFlight_ || index >= states_.size()) {
    return;
  }
  if (!status) {
    log().debug << "Upstream [" << index << "] did not answer STATUS: " << status.error().message;
  } else if (!hooks_.admit || hooks_.admit(index, status.value())) {
    states_[index] = std::move(status.value());
  }
  if (--pending_ == 0) {
    finishRound();
  }
}

void UpstreamSet::finishRound() {
  // Still in flight while hooks run: a certificate learnt here asks for another round (again_).
  for (size_t i = 0; i < states_.size(); ++i) {
    if (states_[i] && hooks_.learn) {
      hooks_.learn(i, *states_[i]);
    }
  }
  offerConflictingTips();
  chosen_ = choose(states_, hooks_.mayFollow, avoided(), active_);
  if (!chosen_) {
    log().warning << "No upstream to follow (" << why_ << "): none answered with a tip worth following";
  } else if (*chosen_ != active_) {
    const auto &tip = states_[*chosen_]->signedTip;
    follow(*chosen_, "beacon generation " + std::to_string(tip ? tip->generation : 0) + ", nextBlockId " +
                         std::to_string(states_[*chosen_]->nextBlockId) + " (" + why_ + ")");
  }
  inFlight_ = false;
  if (again_) {
    again_ = false;
    rerank(why_);
  }
}

void UpstreamSet::offerConflictingTips() {
  // If both verify, the beacon key signed twice (ba6): two hosts running it, or a stolen key.
  if (!hooks_.offerEquivocation) {
    return;
  }
  for (size_t i = 0; i < states_.size(); ++i) {
    for (size_t j = i + 1; j < states_.size(); ++j) {
      if (!states_[i] || !states_[j] || !states_[i]->signedTip || !states_[j]->signedTip) {
        continue;
      }
      if (const auto evidence = Client::Equivocation::of(*states_[i]->signedTip, *states_[j]->signedTip)) {
        hooks_.offerEquivocation(*evidence, "upstreams [" + std::to_string(i) + "] and [" + std::to_string(j) + "]");
      }
    }
  }
}

void UpstreamSet::noteFailed() {
  if (upstreams_.size() < 2) {
    return;
  }
  avoidUntil_[active_] = Clock::now() + kAvoidFailedFor;
  const auto avoid = avoided();
  for (size_t step = 1; step < upstreams_.size(); ++step) {
    const size_t next = (active_ + step) % upstreams_.size();
    if (avoid.count(next) == 0) {
      follow(next, "the sync from [" + std::to_string(active_) + "] failed");
      break;
    }
  }
  rerank("the sync failed");
}

void UpstreamSet::suspect(const std::string &why) {
  if (upstreams_.size() > 1 && !inFlight_ && Clock::now() - lastRanked_ >= kSuspectGap) {
    rerank(why);
  }
}

void UpstreamSet::maintain() {
  if (upstreams_.size() > 1 && !inFlight_ && Clock::now() - lastRanked_ >= kRerankInterval) {
    rerank("periodic");
  }
}

} // namespace pp
