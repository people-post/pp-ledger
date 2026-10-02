#include "AmpLedgerTransport.h"

#include "../network/LedgerAmpRuntime.h"
#include "../network/LedgerRpcProtocol.h"
#include "amp/L3/ChannelSession.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace pp {
namespace {

using pp::ledger::rpc::kProtocolId;
using pp::ledger::rpc::LedgerRpcChannelPolicy;
using Clock = std::chrono::steady_clock;
using TransportRoe = ILedgerTransport::Roe<std::string>;

/** Extra wall time a threaded waiter allows past the io-lane timeout before giving up. */
constexpr std::chrono::milliseconds kWaiterGrace{1000};

TransportRoe TransportError(const std::string& message) { return LedgerTransportError(-1, message); }

/** One-shot result slot shared by the io lane (producer) and the waiting caller. */
template <typename T> class ResultSlot {
public:
  /** First result wins; later ones are dropped. Returns true if this call set it. */
  bool set(T value) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (value_) {
        return false;
      }
      value_ = std::move(value);
    }
    cv_.notify_all();
    return true;
  }

  bool isSet() const {
    std::lock_guard<std::mutex> lock(mu_);
    return value_.has_value();
  }

  bool waitUntil(Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_until(lock, deadline, [this]() { return value_.has_value(); });
  }

  T take() {
    std::lock_guard<std::mutex> lock(mu_);
    return std::move(*value_);
  }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::optional<T> value_;
};

/**
 * One ledger RPC round trip, run entirely on the io lane:
 * EnsureAssociation → OpenChannel → WhenChannelOpen → BindChannel + send → first frame.
 * Callbacks hold a shared_ptr, so late ones after a timeout are harmless no-ops.
 */
class RoundTripCall : public std::enable_shared_from_this<RoundTripCall> {
public:
  enum class Stage { Association, ChannelOpen, ChannelReady, Response };

  using Done = ILedgerTransport::Done;

  RoundTripCall(pp::amp::MeshRuntime& mesh, std::string peer_key, std::string body,
                std::chrono::milliseconds timeout, Done done = {})
      : mesh_(mesh), peer_key_(std::move(peer_key)), body_(std::move(body)), timeout_(timeout),
        done_(std::move(done)) {}

  ResultSlot<TransportRoe>& result() { return result_; }

  /** io lane. */
  void start() {
    auto self = shared_from_this();
    deadline_ms_ = mesh_.GetEndpoint().GetClock().NowMs() + timeout_.count();
    timer_ = mesh_.PostAfter(timeout_, [self]() { self->expire(); });
    mesh_.EnsureAssociation(peer_key_, [self](pp::amp::PeerLinkManager::LinkRoe r) { self->onAssociated(r); });
  }

  /** io lane. Fail with the timeout for the current stage. */
  void expire() {
    timer_ = 0;
    finish(TransportError(timeoutMessage()));
  }

  /** Any thread: the waiter gave up. Resolves the result, then cleans up on the io lane. */
  void abandon(const std::function<void(std::function<void()>)>& post) {
    if (!resolve(TransportError(timeoutMessage()))) {
      return;
    }
    auto self = shared_from_this();
    post([self]() { self->cleanup(); });
  }

private:
  const char* timeoutMessage() const {
    switch (stage_.load()) {
    case Stage::Association:
      return "AmpLedgerTransport: association timeout";
    case Stage::ChannelOpen:
      return "AmpLedgerTransport: channel open timeout";
    case Stage::ChannelReady:
      return "AmpLedgerTransport: channel not open";
    case Stage::Response:
      break;
    }
    return "AmpLedgerTransport: response timeout";
  }

  void onAssociated(const pp::amp::PeerLinkManager::LinkRoe& r) {
    if (result_.isSet()) {
      return;
    }
    if (!r) {
      const std::string& message = r.error().message;
      finish(TransportError(message.empty() ? "AmpLedgerTransport: association failed" : message));
      return;
    }
    if (!mesh_.IsConnected(peer_key_)) {
      finish(TransportError("AmpLedgerTransport: association not connected"));
      return;
    }
    stage_ = Stage::ChannelOpen;
    auto self = shared_from_this();
    mesh_.OpenChannel(peer_key_, kProtocolId, LedgerRpcChannelPolicy(timeout_),
                      [self](pp::amp::PeerLinkManager::ChannelRoe ch) { self->onChannel(ch); });
  }

  void onChannel(const pp::amp::PeerLinkManager::ChannelRoe& ch) {
    if (result_.isSet()) {
      return;
    }
    if (!ch) {
      const std::string& message = ch.error().message;
      finish(TransportError(message.empty() ? "AmpLedgerTransport: channel open failed" : message));
      return;
    }
    channel_id_ = ch.value();
    stage_ = Stage::ChannelReady;
    auto self = shared_from_this();
    mesh_.WhenChannelOpen(peer_key_, channel_id_, deadline_ms_, [self](bool ok) { self->onChannelOpen(ok); });
  }

  void onChannelOpen(const bool ok) {
    if (result_.isSet()) {
      return;
    }
    if (!ok) {
      finish(TransportError("AmpLedgerTransport: channel not open"));
      return;
    }
    stage_ = Stage::Response;
    auto self = shared_from_this();
    session_ = mesh_.BindChannel(peer_key_, channel_id_, LedgerRpcChannelPolicy(timeout_),
                                 [self](pp::Roe<std::vector<uint8_t>> body) { return self->onFrame(std::move(body)); });
    if (!session_) {
      finish(TransportError("AmpLedgerTransport: link unavailable"));
      return;
    }
    bound_mux_ = session_->Mux();
    std::vector<uint8_t> request(body_.begin(), body_.end());
    if (!session_->EnqueueOutbound(std::move(request))) {
      finish(TransportError("AmpLedgerTransport: failed to enqueue request"));
    }
  }

  bool onFrame(pp::Roe<std::vector<uint8_t>> body) {
    if (!body) {
      finish(TransportError(body.error().message));
      return false;
    }
    if (body->size() > pp::ledger::rpc::kMaxPayloadBytes) {
      finish(TransportError("AmpLedgerTransport: response too large"));
      return false;
    }
    finish(std::string(body->begin(), body->end()));
    return true;
  }

  /** First result wins: store it for a waiter and hand it to `done_`. */
  bool resolve(TransportRoe result) {
    if (!done_) {
      return result_.set(std::move(result));
    }
    if (!result_.set(result)) {
      return false;
    }
    done_(std::move(result));
    return true;
  }

  /** io lane. */
  void finish(TransportRoe result) {
    if (!resolve(std::move(result))) {
      return;
    }
    cleanup();
  }

  /** io lane. Cancel the timer; detach the session on the teardown lane (off the mux stack). */
  void cleanup() {
    if (timer_ != 0) {
      mesh_.CancelTimer(timer_);
      timer_ = 0;
    }
    if (!session_) {
      return;
    }
    auto self = shared_from_this();
    mesh_.PostDeferred([self]() { self->detachSession(); });
  }

  /** Unbind safely: if the PeerLink/Mux was torn down, do not touch the dangling mux. */
  void detachSession() {
    auto session = std::move(session_);
    if (!session) {
      return;
    }
    const bool live = mesh_.WithLiveLinkByDialKey(peer_key_, [&](pp::amp::PeerLink& link) {
      if (bound_mux_ && link.Mux() == bound_mux_) {
        if (!session->IsClosed()) {
          session->CloseQuiet();
        }
        session->ReleaseHandlers();
      } else {
        session->OrphanFromMux();
      }
    });
    if (!live) {
      session->OrphanFromMux();
    }
  }

  pp::amp::MeshRuntime& mesh_;
  const std::string peer_key_;
  const std::string body_;
  const std::chrono::milliseconds timeout_;
  const Done done_;
  ResultSlot<TransportRoe> result_;
  // io-lane state (stage_ is also read by a waiter building its timeout message).
  std::atomic<Stage> stage_{Stage::Association};
  int64_t deadline_ms_{0};
  pp::amp::MeshRuntime::TimerId timer_{0};
  uint32_t channel_id_{0};
  std::shared_ptr<pp::amp::ChannelSession> session_;
  pp::amp::ChannelMux* bound_mux_{nullptr};
};

} // namespace

AmpLedgerTransport::AmpLedgerTransport(network::LedgerAmpRuntime& runtime, std::string peer_key)
    : mesh_(runtime.runtime()), runtime_(&runtime), peer_key_(std::move(peer_key)) {}

AmpLedgerTransport::AmpLedgerTransport(pp::amp::MeshRuntime& mesh, std::string peer_key, DriveFn drive)
    : mesh_(mesh), drive_(std::move(drive)), peer_key_(std::move(peer_key)) {}

void AmpLedgerTransport::post(std::function<void()> task) {
  if (runtime_) {
    runtime_->post(std::move(task));
  } else {
    mesh_.PostToIo(std::move(task));
  }
}

bool AmpLedgerTransport::registerEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  if (runtime_ && !runtime_->isRunning()) {
    return false;
  }
  if (runtime_ && runtime_->onPumpThread()) {
    return mesh_.RegisterEndpoint(peer_key, multiaddr).isOk();
  }
  auto done = std::make_shared<ResultSlot<bool>>();
  post([&mesh = mesh_, done, peer_key, multiaddr]() {
    done->set(mesh.RegisterEndpoint(peer_key, multiaddr).isOk());
  });
  const auto deadline = Clock::now() + kWaiterGrace;
  if (drive_) {
    while (!done->isSet() && Clock::now() < deadline) {
      drive_();
    }
  } else {
    done->waitUntil(deadline);
  }
  // Resolve a timed-out wait as failure; a late io-lane result is then dropped.
  done->set(false);
  return done->take();
}

AmpLedgerTransport::Roe<std::string> AmpLedgerTransport::roundTrip(const std::string& requestBody,
                                                                   const std::chrono::milliseconds timeout) {
  if (peer_key_.empty()) {
    return TransportError("AmpLedgerTransport: peer_key not set");
  }
  if (requestBody.size() > pp::ledger::rpc::kMaxPayloadBytes) {
    return TransportError("AmpLedgerTransport: request too large");
  }
  if (runtime_ && !runtime_->isRunning()) {
    return TransportError("AmpLedgerTransport: AMP runtime not running");
  }
  if (runtime_ && runtime_->onPumpThread()) {
    return TransportError("AmpLedgerTransport: roundTrip on the AMP pump thread would deadlock");
  }

  auto call = std::make_shared<RoundTripCall>(mesh_, peer_key_, requestBody, timeout);
  post([call]() { call->start(); });

  const auto postTask = [this](std::function<void()> task) { post(std::move(task)); };
  if (drive_) {
    // Caller-driven: this thread is the driver. The amp clock may be virtual,
    // so bound the wait on wall time here rather than relying on the io timer.
    const auto deadline = Clock::now() + timeout;
    while (!call->result().isSet() && Clock::now() < deadline) {
      drive_();
    }
    if (!call->result().isSet()) {
      call->abandon(postTask);
      drive_();
    }
  } else if (!call->result().waitUntil(Clock::now() + timeout + kWaiterGrace)) {
    // The io-lane timer normally resolves first; this covers a stalled pump thread.
    call->abandon(postTask);
  }
  return call->result().take();
}

void AmpLedgerTransport::roundTripAsync(const std::string& requestBody, const std::chrono::milliseconds timeout,
                                        Done done) {
  if (peer_key_.empty()) {
    done(TransportError("AmpLedgerTransport: peer_key not set"));
    return;
  }
  if (requestBody.size() > pp::ledger::rpc::kMaxPayloadBytes) {
    done(TransportError("AmpLedgerTransport: request too large"));
    return;
  }
  if (runtime_ && !runtime_->isRunning()) {
    done(TransportError("AmpLedgerTransport: AMP runtime not running"));
    return;
  }
  // The io-lane amp timer bounds the call; no thread waits on it.
  auto call = std::make_shared<RoundTripCall>(mesh_, peer_key_, requestBody, timeout, std::move(done));
  post([call]() { call->start(); });
}

} // namespace pp
