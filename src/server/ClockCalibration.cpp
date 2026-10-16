#include "ClockCalibration.h"

#include <memory>

namespace pp::clock_calibration {

namespace {

struct Run : std::enable_shared_from_this<Run> {
  Fetch fetch;
  Done done;
  Offset best;
  std::chrono::steady_clock::time_point sentAt;

  void sample() {
    sentAt = std::chrono::steady_clock::now();
    auto self = shared_from_this();
    fetch([self](Client::Roe<Client::CalibrationResponse> response) { self->onResponse(std::move(response)); });
  }

  void onResponse(Client::Roe<Client::CalibrationResponse> response) {
    if (!response) {
      done(Client::Roe<Offset>(response.error()));
      return;
    }
    const int64_t rttMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sentAt).count();
    const int64_t offsetMs = offsetOf(response.value().msTimestamp, localNowMs(), rttMs);
    if (best.samples == 0 || rttMs < best.rttMs) {
      best.offsetMs = offsetMs;
      best.rttMs = rttMs;
    }
    ++best.samples;
    if (rttMs <= kPreciseRtt.count() || best.samples >= kMaxSamples) {
      done(Client::Roe<Offset>(best));
      return;
    }
    sample();
  }
};

} // namespace

int64_t offsetOf(int64_t serverMs, int64_t localReceiptMs, int64_t rttMs) {
  return serverMs + rttMs / 2 - localReceiptMs;
}

int64_t localNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void estimate(Fetch fetch, Done done) {
  auto run = std::make_shared<Run>();
  run->fetch = std::move(fetch);
  run->done = std::move(done);
  run->sample();
}

ClockFollower::ClockFollower(Hooks hooks) : hooks_(std::move(hooks)) { redirectLogger("ClockFollower"); }

void ClockFollower::maintain() {
  const uint64_t generation = hooks_.generation();
  if (inFlight_ || generation <= calibratedGeneration_ || std::chrono::steady_clock::now() < notBefore_) {
    return;
  }
  inFlight_ = true;
  estimate(hooks_.fetch, [this, generation](Client::Roe<Offset> offset) {
    inFlight_ = false;
    if (!offset) {
      log().warning << "Recalibration for beacon generation " << generation << " failed (retry in "
                    << kRetryAfter.count() << " s): " << offset.error().message;
      notBefore_ = std::chrono::steady_clock::now() + kRetryAfter;
      return;
    }
    calibratedGeneration_ = generation;
    log().info << "Clock recalibrated for beacon generation " << generation << ": offset " << offset.value().offsetMs
               << " ms (round trip " << offset.value().rttMs << " ms)";
    hooks_.apply(offset.value().offsetMs);
  });
}

} // namespace pp::clock_calibration
