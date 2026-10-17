#include "FollowerServer.h"
#include "../chain/PredecessorCheck.h"
#include "../network/amp/AmpIdentity.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Logger.h"

#include <algorithm>

namespace pp {

FollowerServer::FollowerServer(const std::string &name) {
  redirectLogger(name);
  client_.redirectLogger(log().getFullName() + ".Client");
  syncClient_.redirectLogger(log().getFullName() + ".SyncClient");
  fanoutClient_.redirectLogger(log().getFullName() + ".FanoutClient");
}

void FollowerServer::bindFollower(FollowerCore &core) {
  follower_ = &core;
  bindCore(core);
  handleLater(Client::T_REQ_BLOCK_GET,
              [this](const Client::Request &request, const RequestQueue::Reply &reply) { dBlockGet(request, reply); });
  handleLater(Client::T_REQ_TX_ADD,
              [this](const Client::Request &request, const RequestQueue::Reply &reply) { dTxAdd(request, reply); });
  handleLater(Client::T_REQ_EQUIVOCATION_ADD, [this](const Client::Request &request, const RequestQueue::Reply &reply) {
    dEquivocationAdd(request, reply);
  });
}

// ============ start ============

Service::Roe<void> FollowerServer::startFollowing(const FollowerConfig &config, const std::string &identityKey) {
  config_ = config;
  if (auto set = setUpstreams(config_.upstreams); !set) {
    return Error(E_CONFIG, set.error().message);
  }
  initUpstreams();

  auto ampCfg = network::LedgerAmpConfigFromPrivateKey(identityKey, config_.port);
  if (!ampCfg) {
    return Error(E_CONFIG, "Failed to build AMP config: " + ampCfg.error().message);
  }
  setAllowedPeers(config_.allowedPeers);
  setLinkLimits(config_);
  if (auto started = startAmpServer(*ampCfg); !started) {
    return Error(E_NETWORK, "Failed to start AMP server: " + started.error().message);
  }
  if (!ampRuntime()) {
    return Error(E_NETWORK, "AMP runtime unavailable after start");
  }
  client_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  syncClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  fanoutClient_.attachAmpTransport(*ampRuntime(), UpstreamSet::dialKey(0));
  return {};
}

Service::Roe<void> FollowerServer::admitUpstream(const Client::BeaconState &state) const {
  if (auto anchored = NetworkAnchor::verifyStatus<Error>(config_.anchor, state); !anchored) {
    return anchored;
  }
  const std::string networkId = follower().getNetworkId();
  if (!networkId.empty() && state.networkId != networkId) {
    return Error(E_CONFIG, "it serves network '" + state.networkId + "', not this chain's '" + networkId + "'");
  }
  return {};
}

std::optional<std::string> FollowerServer::firstBlockHash() const {
  return config_.anchor.genesis_hash.empty() ? std::nullopt : std::optional<std::string>(config_.anchor.genesis_hash);
}

void FollowerServer::initUpstreams() {
  UpstreamSet::Hooks hooks = upstreamHooks(fanoutClient_);
  hooks.admit = [this](size_t index, const Client::BeaconState &state) {
    if (auto admitted = admitUpstream(state); !admitted) {
      log().warning << "Disregarding upstream [" << index << "]: " << admitted.error().message;
      return false;
    }
    return true;
  };
  hooks.learn = [this](size_t index, const Client::BeaconState &state) {
    const std::string from = "upstream [" + std::to_string(index) + "]'s STATUS";
    if (state.certificate) {
      noteCertificate(*state.certificate, from);
    }
    if (state.equivocation) {
      noteEquivocation(*state.equivocation, from, /*pushUp=*/false);
    }
  };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence, const std::string &from) {
    noteEquivocation(evidence, from, /*pushUp=*/true);
  };
  hooks.mayFollow = [this](const Client::SignedTip &tip) { return follower().mayFollowTip(tip); };
  upstreams_ = std::make_unique<UpstreamSet>(config_.upstreams, std::move(hooks));
  upstreams_->redirectLogger(log().getFullName() + ".Upstreams");
}

Service::Roe<int64_t> FollowerServer::calibrateTimeToBeacon() {
  // The upstream followed first, then any other that answers.
  Roe<int64_t> offset = Error(E_NETWORK, "No upstream configured");
  const size_t count = upstreams_->all().size();
  for (size_t step = 0; step < count; ++step) {
    const size_t i = (upstreams_->activeIndex() + step) % count;
    auto estimated = calibrateBlocking(calibrationFetch(client_, UpstreamSet::dialKey(i), upstreams_->all()[i]));
    if (estimated) {
      log().info << "Time calibrated through upstream [" << i << "]: offset " << estimated.value().offsetMs
                 << " ms (round trip " << estimated.value().rttMs << " ms, " << estimated.value().samples
                 << " sample(s))";
      return estimated.value().offsetMs;
    }
    log().debug << "Time calibration through upstream [" << i << "] failed: " << estimated.error().message;
    offset = Error(E_NETWORK, estimated.error().message);
  }
  return offset;
}

void FollowerServer::applyTimeOffset(int64_t offsetMs) {
  timeOffsetToBeaconMs_ = offsetMs;
  follower().setTimeOffsetMs(offsetMs);
}

bool FollowerServer::learnUpstreamHints() { return upstreams_ && upstreams_->addHints(follower().getDialHints()) > 0; }

std::string FollowerServer::catchUp(const std::function<bool()> &extraDone) {
  initClockFollower();
  initBlockSync();
  lastSyncResult_.reset();
  return runStartupSync(*blockSync_, lastSyncResult_, networkTuning().startupSyncTimeout, extraDone);
}

void FollowerServer::initClockFollower() {
  clock_calibration::ClockFollower::Hooks hooks;
  hooks.generation = [this]() { return follower().getGeneration(); };
  hooks.fetch = [this](Client::Done<Client::CalibrationResponse> done) {
    calibrationFetch(client_, UpstreamSet::dialKey(upstreams_->activeIndex()), upstreams_->active())(std::move(done));
  };
  hooks.apply = [this](int64_t offsetMs) { applyTimeOffset(offsetMs); };
  clock_ = std::make_unique<clock_calibration::ClockFollower>(std::move(hooks));
  clock_->redirectLogger(log().getFullName() + ".Clock");
  clock_->calibratedFor(follower().getGeneration());
}

void FollowerServer::initBlockSync() {
  BlockSync::Hooks hooks;
  hooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialUpstream(syncClient_, upstreams_->activeIndex());
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  hooks.nextBlockId = [this]() { return follower().getNextBlockId(); };
  hooks.tipHash = [this]() { return follower().getTipHash(); };
  hooks.firstBlockHash = [this]() { return firstBlockHash(); };
  if (const std::string dir = config_.anchor.predecessor_dir; !dir.empty()) {
    hooks.checkGenesis = [this, dir](const Ledger::ChainNode &genesis) {
      auto checked = predecessor_check::check(genesis, dir);
      if (checked) {
        log().info << "Re-genesis checked against the old ledger in " << dir;
      }
      return checked;
    };
  }
  hooks.checkStartingAuthority = [this](uint64_t blockId, const ChainAuthority &authority,
                                        const Client::Wallet &admin) {
    return checkStartingAuthority(blockId, authority, admin);
  };
  hooks.authorityTrail = [this]() -> pp::Roe<AuthorityTrail> {
    auto trail = follower().authorityTrail();
    return trail ? pp::Roe<AuthorityTrail>(trail.value()) : pp::Roe<AuthorityTrail>(pp::Error(trail.error().message));
  };
  hooks.onTipVerified = [this](const Client::SignedTip &tip) { onSignedTip(tip); };
  hooks.offerCertificate = [this](const Ledger::Record &record) { noteCertificate(record, "the upstream's STATUS"); };
  hooks.offerEquivocation = [this](const Client::Equivocation &evidence) {
    noteEquivocation(evidence, "the upstream's STATUS", /*pushUp=*/false);
  };
  hooks.vouchedCutover = [this]() { return follower().vouchedCutover(); };
  hooks.applyBlock = [this](const Ledger::ChainNode &fetched) -> pp::Roe<void> {
    Ledger::ChainNode block = fetched;
    block.hash = follower().calculateHash(block.block);
    auto added = follower().addBlock(block);
    return added ? pp::Roe<void>() : pp::Roe<void>(pp::Error(added.error().message));
  };
  hooks.postToServerThread = [this](std::function<void()> task) { postToServerThread(std::move(task)); };
  hooks.onFinished = [this](const BlockSync::Result &result) { onBlockSyncFinished(result); };
  UpstreamTipWatch::Hooks watchHooks;
  watchHooks.dialUpstream = [this]() -> pp::Roe<void> {
    auto dial = dialUpstream();
    return dial ? pp::Roe<void>() : pp::Roe<void>(pp::Error(dial.error().message));
  };
  watchHooks.nextBlockId = hooks.nextBlockId;
  watchHooks.onUpstreamAhead = [this]() { requestSync(); };
  watchHooks.onSignedTip = [this](const Client::SignedTip &tip) { onSignedTip(tip); };
  watchHooks.postToServerThread = hooks.postToServerThread;
  blockSync_ = std::make_unique<BlockSync>(syncClient_, std::move(hooks));
  blockSync_->redirectLogger(log().getFullName() + ".BlockSync");
  tipWatch_ = std::make_unique<UpstreamTipWatch>(client_, std::move(watchHooks));
  tipWatch_->redirectLogger(log().getFullName() + ".TipWatch");
}

pp::Roe<void> FollowerServer::checkStartingAuthority(uint64_t /*blockId*/, const ChainAuthority & /*authority*/,
                                                     const Client::Wallet & /*admin*/) const {
  return {};
}

void FollowerServer::onSignedTip(const Client::SignedTip & /*tip*/) {}

// ============ run ============

void FollowerServer::runLoop() {
  log().info << "Run loop started";
  while (!isStopSet()) {
    try {
      orderRewindIfDue();
      follower().refresh();
      syncBlocksPeriodically();
      tipWatch_->maintain();
      upstreams_->maintain();
      clock_->maintain();
      if (blockSyncRequested_) {
        blockSyncRequested_ = false;
        trySyncBlocksFromBeacon(true);
      }
      roleDuties();
      serveRequestsFor(std::chrono::milliseconds(100));
    } catch (const std::exception &e) {
      log().error << "Exception in the run loop: " << e.what();
      serveRequestsFor(std::chrono::seconds(1));
    }
  }
  log().info << "Run loop stopped";
}

void FollowerServer::onStop() {
  // Reply before AMP stops (Server::onStop), while replies can still be sent.
  for (auto &pending : pendingBlockGets_) {
    pending.reply(packResponse(Client::E_SERVER_ERROR, "Server stopping"));
  }
  pendingBlockGets_.clear();
  Server::onStop();
  log().info << getServerName() << " resources cleaned up";
}

void FollowerServer::syncBlocksPeriodically() {
  if (follower().getSlotDuration() == 0) {
    return;
  }
  // At each new epoch (the stakeholders change), or when the role needs the tip now.
  if (follower().getCurrentEpoch() > lastSyncedEpoch_ || syncDue()) {
    trySyncBlocksFromBeacon(false);
  }
}

void FollowerServer::trySyncBlocksFromBeacon(bool bypassRateLimit) {
  const uint64_t slotDurationSec = follower().getSlotDuration();
  if (!bypassRateLimit && slotDurationSec > 0) {
    const auto elapsedSec =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastBlockSyncTime_).count();
    if (elapsedSec < static_cast<int64_t>(slotDurationSec)) {
      return; // Rate limit: at most one sync per slot time
    }
  }
  if (syncHeld()) {
    requestSync();
    return;
  }
  if (blockSync_->start() == BlockSync::Start::BackingOff) {
    // No sync coming soon: answer waiting block gets with what we have.
    resolvePendingBlockGets();
  }
}

void FollowerServer::onBlockSyncFinished(const BlockSync::Result &result) {
  lastSyncResult_ = result;
  orderRewindIfDue(); // the sync may have brought a certificate whose cutover is below our tip
  if (result.ok) {
    lastBlockSyncTime_ = std::chrono::steady_clock::now();
    lastSyncedEpoch_ = follower().getCurrentEpoch();
    onSynced();
  } else {
    upstreams_->noteFailed(); // the next attempt (after backoff) tries another upstream
  }
  resolvePendingBlockGets();
}

void FollowerServer::fillHealth(Client::NodeHealth &health) const {
  Server::fillHealth(health);
  if (health.signedTipHeight) {
    health.upstreamNextBlockId = *health.signedTipHeight + 1;
  }
  const bool synced = lastSyncResult_ && lastSyncResult_->ok;
  if (synced) {
    health.lastSyncAgeSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastBlockSyncTime_).count();
  }
  if (!isServing()) {
    return;
  }
  const uint64_t lag = health.upstreamNextBlockId && *health.upstreamNextBlockId > health.nextBlockId
                           ? *health.upstreamNextBlockId - health.nextBlockId
                           : 0;
  if (health.halted) {
    health.notReady = "halted on equivocation evidence";
  } else if (!synced) {
    health.notReady = "not synced from the upstream";
  } else if (lag > kReadyLagBlocks) {
    health.notReady = std::to_string(lag) + " blocks behind the upstream";
  } else {
    health.ready = true;
  }
}

// ============ upstreams ============

Client::Roe<void> FollowerServer::dialUpstream(Client &client, size_t index) {
  if (!upstreams_ || index >= upstreams_->all().size()) {
    return Client::Error(Client::E_NOT_CONNECTED, "No upstream [" + std::to_string(index) + "]");
  }
  return client.setAmpPeer(UpstreamSet::dialKey(index), upstreams_->all()[index]);
}

Client::Roe<void> FollowerServer::dialUpstream() {
  return dialUpstream(client_, upstreams_ ? upstreams_->activeIndex() : 0);
}

bool FollowerServer::dialUpstreamFor(const RequestQueue::Reply &reply) {
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message)));
    return false;
  }
  return true;
}

std::chrono::milliseconds FollowerServer::forwardBudget(const Client::Request &request) {
  if (request.deadlineMs == 0) {
    return network::NetworkTuning::dataTimeoutFor(network::NetworkTuning::kDefaultRpcTimeout) / 2;
  }
  const auto deadline = std::chrono::milliseconds(request.deadlineMs);
  const auto margin = std::clamp(deadline / 10, std::chrono::milliseconds(250), std::chrono::milliseconds(2000));
  return deadline > margin ? deadline - margin : std::chrono::milliseconds(0);
}

std::optional<std::chrono::milliseconds> FollowerServer::forwardOrExpire(const Client::Request &request,
                                                                         const RequestQueue::Reply &reply) {
  const auto budget = forwardBudget(request);
  if (budget < kMinForward) {
    reply(packError(Client::E_EXPIRED, "No time left to forward the request within its deadline"));
    return std::nullopt;
  }
  return budget;
}

void FollowerServer::replyUpstreamError(const RequestQueue::Reply &reply, const Client::Error &error,
                                        const std::string &prefix) {
  if (Client::isOverloadCode(error.code)) {
    reply(packError(static_cast<uint16_t>(error.code), prefix + error.message, error.retryAfterMs));
  } else if (Client::isTransportError(error.code) && error.message.find("response timeout") != std::string::npos) {
    reply(packError(Client::E_UPSTREAM_TIMEOUT,
                    prefix + "The upstream did not answer in time: it may have taken the request; sending the same "
                             "bytes again is safe"));
  } else if (Client::isTransportError(error.code)) {
    reply(packError(Client::E_BUSY, prefix + "Upstream unreachable: " + error.message, kBusyRetryAfterMs));
  } else {
    reply(packResponse(1, prefix + error.message));
  }
}

std::optional<Chain::CertificateOffer> FollowerServer::noteCertificate(const Ledger::Record &record,
                                                                       const std::string &from) {
  auto offered = follower().noteCertificate(record, from);
  if (offered == Chain::CertificateOffer::Newer) {
    onTrustChanged();
    if (upstreams_) {
      // bp2b: up the tree, to the new beacon's gateways, and follow whichever serves the new generation.
      pushCertificateUp(record);
      learnUpstreamHints();
      upstreams_->rerank("a new beacon certificate");
    }
  }
  return offered;
}

std::optional<Chain::EquivocationOffer> FollowerServer::noteEquivocation(const Client::Equivocation &evidence,
                                                                         const std::string &from, bool pushUp) {
  auto offered = follower().noteEquivocation(evidence, from);
  if (offered == Chain::EquivocationOffer::Newer) {
    onTrustChanged();
    if (pushUp) {
      pushEquivocationUp(evidence);
    }
  }
  return offered;
}

void FollowerServer::pushCertificateUp(const Ledger::Record &record) {
  for (size_t i = 0; i < upstreams_->all().size(); ++i) {
    if (auto dial = dialUpstream(fanoutClient_, i); !dial) {
      log().warning << "Certificate not passed to upstream [" << i << "]: " << dial.error().message;
      continue;
    }
    fanoutClient_.addTransactionAsync(record, completeOnServerThread<void>([this, i](Client::Roe<void> result) {
      if (!result) {
        log().debug << "Upstream [" << i << "] did not take the certificate: " << result.error().message;
      }
    }));
  }
}

void FollowerServer::pushEquivocationUp(const Client::Equivocation &evidence) {
  // To every upstream: each side of a split runs its own beacon host, and each must halt. Downstreams
  // learn it from this node's STATUS.
  for (size_t i = 0; i < upstreams_->all().size(); ++i) {
    if (auto dial = dialUpstream(fanoutClient_, i); !dial) {
      log().warning << "Equivocation evidence not passed to upstream [" << i << "]: " << dial.error().message;
      continue;
    }
    fanoutClient_.addEquivocationAsync(evidence,
                                       completeOnServerThread<std::string>([this, i](Client::Roe<std::string> result) {
                                         if (!result) {
                                           log().warning << "Equivocation evidence not passed to upstream [" << i
                                                         << "]: " << result.error().message;
                                         }
                                       }));
  }
}

// ============ requests ============

void FollowerServer::dBlockGet(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto idResult = utl::binaryUnpack<uint64_t>(request.payload);
  if (!idResult) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid block get payload: " + request.payload)));
    return;
  }
  const uint64_t blockId = idResult.value();
  auto result = follower().readBlock(blockId);
  if (result) {
    reply(packResponse(result.value().ltsToString()));
    return;
  }
  if (blockId < follower().getNextBlockId()) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    return;
  }
  // Beyond our tip: answer after the run loop's next sync instead of syncing here.
  pendingBlockGets_.push_back({blockId, reply});
  requestSync();
}

void FollowerServer::resolvePendingBlockGets() {
  for (auto &pending : pendingBlockGets_) {
    auto result = follower().readBlock(pending.blockId);
    if (result) {
      pending.reply(packResponse(result.value().ltsToString()));
    } else {
      replyWith(pending.reply, Roe<std::string>(Error(E_REQUEST, "Failed to get block: " + result.error().message)));
    }
  }
  pendingBlockGets_.clear();
}

void FollowerServer::dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  auto record = utl::binaryUnpack<Ledger::Record>(request.payload);
  if (!record) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Failed to deserialize transaction: " + record.error().message)));
    return;
  }
  if (checksTransactions() && record.value().type != Ledger::T_BEACON_CERT) {
    const int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (auto admitted = follower().checkAdmission(record.value(), now, /*verifySignatures=*/true); !admitted) {
      replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Transaction refused: " + admitted.error().message)));
      return;
    }
  }
  // A beacon certificate counts here too (ba5c): a node cut off from an honest beacon still keeps it.
  std::string kept;
  if (record.value().type == Ledger::T_BEACON_CERT) {
    if (const auto offered = noteCertificate(record.value(), "a TX_ADD");
        offered == Chain::CertificateOffer::Newer || offered == Chain::CertificateOffer::Same) {
      kept = "Beacon certificate kept";
    }
  }
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, kept.empty() ? Roe<std::string>(Error(E_NETWORK, "Failed to dial upstream: " + dial.error().message))
                                  : Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  // Up the tree to the beacon's pool, where the slot leader pulls it: miners never need each other's
  // addresses. The reply text is the beacon's (submitted, or already submitted).
  client_.requestAsync(
      Client::T_REQ_TX_ADD, request.payload,
      completeOnServerThread<std::string>([reply, kept](Client::Roe<std::string> result) {
        if (result) {
          reply(packResponse(result.value()));
        } else {
          replyUpstreamError(reply, result.error(), kept.empty() ? "" : kept + "; the upstream: ");
        }
      }),
      *budget);
}

void FollowerServer::dEquivocationAdd(const Client::Request &request, const RequestQueue::Reply &reply) {
  Client::Equivocation evidence;
  if (!evidence.ltsFromString(request.payload)) {
    replyWith(reply, Roe<std::string>(Error(E_REQUEST, "Invalid equivocation evidence")));
    return;
  }
  const auto offered = noteEquivocation(evidence, "an EQUIVOCATION_ADD", /*pushUp=*/false);
  const std::string kept = offered == Chain::EquivocationOffer::Newer || offered == Chain::EquivocationOffer::Same
                               ? "Equivocation evidence kept"
                               : "Equivocation evidence not kept here";
  // Up the tree whatever this node made of it: the beacon judges for itself.
  if (auto dial = dialUpstream(); !dial) {
    replyWith(reply, Roe<std::string>(kept + "; not forwarded: " + dial.error().message));
    return;
  }
  const auto budget = forwardOrExpire(request, reply);
  if (!budget) {
    return;
  }
  client_.addEquivocationAsync(
      evidence,
      completeOnServerThread<std::string>([reply, kept](Client::Roe<std::string> result) {
        replyWith(reply, Roe<std::string>(kept + "; upstream: " + (result ? result.value() : result.error().message)));
      }),
      *budget);
}

} // namespace pp
