#ifndef PP_LEDGER_CLOCK_CALIBRATION_H
#define PP_LEDGER_CLOCK_CALIBRATION_H

#include "../client/Client.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace pp::clock_calibration {

/** The beacon's clock as seen through one upstream: beacon time (ms) = local time (ms) + offsetMs. */
struct Offset {
  int64_t offsetMs{0};
  /** Round trip of the sample kept (its error is at most half of it). */
  int64_t rttMs{0};
  size_t samples{0};
};

/** A round trip shorter than this is precise enough: one sample. */
constexpr std::chrono::milliseconds kPreciseRtt{200};
/** Otherwise up to this many samples, keeping the one with the shortest round trip. */
constexpr size_t kMaxSamples = 5;

/** Ask one upstream for CALIBRATION; `done` runs on the caller's (server) thread. */
using Fetch = std::function<void(Client::Done<Client::CalibrationResponse> done)>;
using Done = std::function<void(Client::Roe<Offset>)>;

/**
 * Estimate the offset to the beacon's clock through `fetch`, one sample after another (beacon
 * authority bp3: at start, when a node follows a new beacon generation, and for the clock check of a
 * beacon taking over). Times in milliseconds; a failed sample fails the estimate.
 */
void estimate(Fetch fetch, Done done);

/** One sample: the server's time, half the round trip later, against the local time on receipt. */
int64_t offsetOf(int64_t serverMs, int64_t localReceiptMs, int64_t rttMs);

/** The local wall clock in milliseconds. */
int64_t localNowMs();

/**
 * Keeps a relay's or miner's clock offset to the beacon (bp3): calibrated at start, and again when
 * the beacon generation in force rises — a new beacon, maybe another host with another clock. A
 * failed estimate is retried after kRetryAfter. Server thread.
 */
class ClockFollower : public Module {
public:
  struct Hooks {
    /** The beacon generation in force. */
    std::function<uint64_t()> generation;
    /** CALIBRATION through the upstream followed now. */
    Fetch fetch;
    /** Use the new offset (the node's slot clock, and what it answers CALIBRATION with). */
    std::function<void(int64_t offsetMs)> apply;
  };

  static constexpr std::chrono::seconds kRetryAfter{30};

  explicit ClockFollower(Hooks hooks);
  /** The offset in use is calibrated for `generation` (at start). */
  void calibratedFor(uint64_t generation) { calibratedGeneration_ = generation; }
  /** Run loop: estimate again once the generation in force is above the one calibrated for. */
  void maintain();

private:
  Hooks hooks_;
  uint64_t calibratedGeneration_{0};
  bool inFlight_{false};
  std::chrono::steady_clock::time_point notBefore_{};
};

} // namespace pp::clock_calibration

#endif // PP_LEDGER_CLOCK_CALIBRATION_H
