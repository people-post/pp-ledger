#include "lib/common/Service.h"

#include <gtest/gtest.h>

#include <atomic>

using namespace pp;

namespace {

/** A service whose start asks to stop (as a SIGTERM during a long startup sync would). */
class StopDuringStart : public Service {
public:
  bool stopInStart = false;
  std::atomic<int> loops{0};
  std::atomic<int> stops{0};

protected:
  Roe<void> onStart() override {
    if (stopInStart) {
      setStop(true);
    }
    return {};
  }
  void runLoop() override {
    while (!isStopSet()) {
      if (++loops > 3) {
        setStop(true);
      }
    }
  }
  void onStop() override { ++stops; }
};

} // namespace

// A stop requested while onStart() runs is kept: the run loop does not start (docker / systemd
// SIGTERM during a node's startup sync used to be lost, leaving it running).
TEST(ServiceTest, StopRequestedDuringStartIsKept) {
  StopDuringStart service;
  service.stopInStart = true;
  ASSERT_TRUE(service.run().isOk());
  EXPECT_EQ(service.loops.load(), 0);
  EXPECT_EQ(service.stops.load(), 1);
  EXPECT_TRUE(service.isStopSet());

  StopDuringStart threaded;
  threaded.stopInStart = true;
  ASSERT_TRUE(threaded.start().isOk());
  EXPECT_EQ(threaded.loops.load(), 0);
}

TEST(ServiceTest, RunsTheLoopWithoutAStop) {
  StopDuringStart service;
  ASSERT_TRUE(service.run().isOk());
  EXPECT_GT(service.loops.load(), 0);
  EXPECT_EQ(service.stops.load(), 1);
}
