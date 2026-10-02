#include "NetworkTuning.h"
#include "common/io/Json.h"

#include <gtest/gtest.h>

using pp::network::NetworkTuning;

namespace {

pp::Roe<NetworkTuning> parse(const std::string &json) {
  auto value = pp::common::io::valueFromJsonString(json);
  const pp::common::Object *config = value.isOk() ? pp::common::asObject(value.value()) : nullptr;
  if (!config) {
    ADD_FAILURE() << "not a JSON object: " << json;
    return pp::Error("bad test input");
  }
  return NetworkTuning::fromConfig(*config);
}

} // namespace

TEST(NetworkTuningTest, AbsentSectionKeepsDefaults) {
  auto t = parse(R"({"port": 8518})");
  ASSERT_TRUE(t) << t.error().message;
  EXPECT_EQ(t->rpcTimeout, NetworkTuning::kDefaultRpcTimeout);
  EXPECT_EQ(t->requestQueueCapacity, NetworkTuning::kDefaultRequestQueueCapacity);
  EXPECT_EQ(t->startupSyncTimeout, NetworkTuning::kDefaultStartupSyncTimeout);
  EXPECT_EQ(t->adp.reliable_window, pp::adp::AdpTuning{}.reliable_window);
  EXPECT_TRUE(NetworkTuning{}.validate());
}

TEST(NetworkTuningTest, QueueExpiryDerivesFromRpcTimeout) {
  auto t = parse(R"({"network": {"rpcTimeoutMs": 20000}})");
  ASSERT_TRUE(t) << t.error().message;
  EXPECT_EQ(t->rpcTimeout, std::chrono::milliseconds(20000));
  EXPECT_EQ(t->serverQueueExpiry(), std::chrono::milliseconds(10000));
}

TEST(NetworkTuningTest, AmpFieldsReachLinkConfig) {
  auto t = parse(R"({"network": {"amp": {"reliableWindow": 64, "replayWindow": 256,
                     "aliveTimeoutMs": 9000, "maxConcurrentChannels": 32}}})");
  ASSERT_TRUE(t) << t.error().message;
  pp::amp::PeerLinkConfig link;
  t->applyTo(link);
  EXPECT_EQ(link.adp.reliable_window, 64u);
  EXPECT_EQ(link.adp.replay_window, 256u);
  EXPECT_EQ(link.adp.alive_timeout_ms, 9000);
  EXPECT_EQ(link.mux.max_concurrent_channels, 32u);
  EXPECT_EQ(link.adp.max_rtx, pp::adp::AdpTuning{}.max_rtx); // untouched
}

TEST(NetworkTuningTest, RejectsWhatCannotWork) {
  for (const char *json : {
           R"({"network": 5})",
           R"({"network": {"rpcTimeoutMs": 500}})",
           R"({"network": {"requestQueueCapacity": 0}})",
           R"({"network": {"rpcTimeoutMs": 20000, "startupSyncTimeoutMs": 10000}})",
           R"({"network": {"rpcTimeout": 15000}})",                  // misspelt
           R"({"network": {"amp": {"reliableWindow": 256}}})",      // replay window too small
           R"({"network": {"amp": {"maxRtx": 99999999999}}})",      // does not fit int
           R"({"network": {"amp": {"window": 1}}})",
           R"({"network": {"amp": 1}})",
       }) {
    EXPECT_FALSE(parse(json)) << json;
  }
}
