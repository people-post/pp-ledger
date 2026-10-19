#include "ledger/server/ClockCalibration.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <thread>

using namespace pp;
using namespace pp::clock_calibration;

// beacon-authority bp3: relays and miners estimate the beacon's clock through an upstream, in
// milliseconds, and estimate it again when a new beacon generation is in force.

namespace {

/** Answers CALIBRATION at once with the local clock shifted by `skewMs`, after `delays` (one per sample). */
struct FakeUpstream {
  int64_t skewMs{0};
  std::deque<std::chrono::milliseconds> delays;
  bool fail{false};
  size_t asked{0};

  Fetch fetch() {
    return [this](Client::Done<Client::CalibrationResponse> done) {
      ++asked;
      if (fail) {
        done(Client::Roe<Client::CalibrationResponse>(Client::Error(Client::E_NOT_CONNECTED, "down")));
        return;
      }
      const auto delay = delays.empty() ? std::chrono::milliseconds(0) : delays.front();
      if (!delays.empty()) {
        delays.pop_front();
      }
      // The round trip: a CI host may sleep longer than asked, so stamp half way through the time
      // that actually passed.
      const int64_t start = localNowMs();
      std::this_thread::sleep_for(delay);
      const int64_t now = localNowMs();
      Client::CalibrationResponse response;
      response.msTimestamp = now + skewMs - (now - start) / 2;
      done(Client::Roe<Client::CalibrationResponse>(response));
    };
  }
};

} // namespace

TEST(ClockCalibrationTest, AnOffsetIsTheServersTimeHalfARoundTripLaterMinusOurs) {
  EXPECT_EQ(offsetOf(10'000, 9'000, 0), 1'000);
  EXPECT_EQ(offsetOf(10'000, 10'100, 200), 0) << "100 ms each way";
  EXPECT_EQ(offsetOf(10'000, 10'500, 0), -500) << "our clock ahead";
}

TEST(ClockCalibrationTest, OneShortRoundTripIsEnough) {
  FakeUpstream upstream;
  upstream.skewMs = 1'500;
  std::optional<Client::Roe<Offset>> result;
  estimate(upstream.fetch(), [&](Client::Roe<Offset> offset) { result = std::move(offset); });
  ASSERT_TRUE(result.has_value() && result->isOk());
  EXPECT_EQ(upstream.asked, 1u);
  EXPECT_LE(std::abs(result->value().offsetMs - 1'500), 50) << "kept to the millisecond, not the second";
}

TEST(ClockCalibrationTest, SlowRoundTripsTakeMoreSamplesAndKeepTheShortest) {
  FakeUpstream upstream;
  upstream.skewMs = -700;
  const auto slow = kPreciseRtt + std::chrono::milliseconds(150);
  // The other samples are far slower, so the shortest stands out however much a host oversleeps.
  upstream.delays = {slow + std::chrono::milliseconds(400), slow + std::chrono::milliseconds(300), slow, slow, slow};
  std::optional<Client::Roe<Offset>> result;
  estimate(upstream.fetch(), [&](Client::Roe<Offset> offset) { result = std::move(offset); });
  ASSERT_TRUE(result.has_value() && result->isOk());
  EXPECT_EQ(upstream.asked, kMaxSamples);
  EXPECT_EQ(result->value().samples, kMaxSamples);
  EXPECT_GE(result->value().rttMs, slow.count());
  EXPECT_LT(result->value().rttMs, slow.count() + 300) << "the shortest round trip";
  EXPECT_LE(std::abs(result->value().offsetMs - (-700)), 50);
}

TEST(ClockCalibrationTest, AFailedSampleFailsTheEstimate) {
  FakeUpstream upstream;
  upstream.fail = true;
  std::optional<Client::Roe<Offset>> result;
  estimate(upstream.fetch(), [&](Client::Roe<Offset> offset) { result = std::move(offset); });
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->isOk());
}

TEST(ClockFollowerTest, EstimatesAgainWhenANewGenerationIsInForce) {
  FakeUpstream upstream;
  upstream.skewMs = 300;
  uint64_t generation = 1;
  std::vector<int64_t> applied;
  ClockFollower::Hooks hooks;
  hooks.generation = [&]() { return generation; };
  hooks.fetch = upstream.fetch();
  hooks.apply = [&](int64_t offsetMs) { applied.push_back(offsetMs); };
  ClockFollower follower(std::move(hooks));
  follower.calibratedFor(1);

  follower.maintain();
  EXPECT_TRUE(applied.empty()) << "calibrated at start for this generation";
  EXPECT_EQ(upstream.asked, 0u);

  generation = 2; // the block committing the new beacon's certificate applied
  follower.maintain();
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_LE(std::abs(applied[0] - 300), 50);
  follower.maintain();
  EXPECT_EQ(applied.size(), 1u) << "once per generation";

  generation = 3;
  upstream.fail = true;
  follower.maintain();
  EXPECT_EQ(applied.size(), 1u);
  const size_t asked = upstream.asked;
  follower.maintain();
  EXPECT_EQ(upstream.asked, asked) << "a failure is retried later, not on every loop";
}
