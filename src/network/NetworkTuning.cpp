#include "NetworkTuning.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

namespace pp {
namespace network {
namespace {

/** Reject keys outside `known`: a misspelt tunable would otherwise be silently ignored. */
pp::Roe<void> checkKnownKeys(const pp::common::Object &jd, const std::string &section,
                             std::initializer_list<const char *> known) {
  for (const auto &[key, value] : jd.fields()) {
    (void)value;
    bool found = false;
    for (const char *k : known) {
      found = found || key == k;
    }
    if (!found) {
      return pp::Error("unknown field '" + section + "." + key + "'");
    }
  }
  return {};
}

/** Read an optional non-negative integer no larger than `max` into `out`. */
template <typename T>
pp::Roe<void> readUInt(const pp::common::Object &jd, const std::string &section, const char *key, T &out,
                       uint64_t max = static_cast<uint64_t>(std::numeric_limits<T>::max())) {
  if (!jd.contains(key)) {
    return {};
  }
  auto v = jd.getNonNegInt(key);
  if (!v || *v > max) {
    return pp::Error("field '" + section + "." + key + "' must be an integer in 0.." + std::to_string(max));
  }
  out = static_cast<T>(*v);
  return {};
}

pp::Roe<void> readMs(const pp::common::Object &jd, const std::string &section, const char *key,
                     std::chrono::milliseconds &out) {
  int64_t ms = out.count();
  if (auto r = readUInt(jd, section, key, ms); !r) {
    return r;
  }
  out = std::chrono::milliseconds(ms);
  return {};
}

pp::Roe<void> readAmp(const pp::common::Object &jd, NetworkTuning &t) {
  const std::string s = "network.amp";
  if (auto r = checkKnownKeys(jd, s,
                              {"reliableWindow", "replayWindow", "rtxIntervalMs", "maxRtx", "skewMs",
                               "aliveTimeoutMs", "maxConcurrentChannels", "maxQueuedBytes",
                               "fragAssemblyTimeoutMs"});
      !r) {
    return r;
  }
  for (auto r : {readUInt(jd, s, "reliableWindow", t.adp.reliable_window),
                 readUInt(jd, s, "replayWindow", t.adp.replay_window),
                 readUInt(jd, s, "rtxIntervalMs", t.adp.rtx_interval_ms),
                 readUInt(jd, s, "maxRtx", t.adp.max_rtx),
                 readUInt(jd, s, "skewMs", t.adp.skew_ms),
                 readUInt(jd, s, "aliveTimeoutMs", t.adp.alive_timeout_ms),
                 readUInt(jd, s, "maxConcurrentChannels", t.mux.max_concurrent_channels),
                 readUInt(jd, s, "maxQueuedBytes", t.mux.max_queued_bytes),
                 readUInt(jd, s, "fragAssemblyTimeoutMs", t.mux.frag_assembly_timeout_ms)}) {
    if (!r) {
      return r;
    }
  }
  return {};
}

} // namespace

void NetworkTuning::applyTo(pp::amp::PeerLinkConfig &config) const {
  config.adp = adp;
  config.mux = mux;
}

pp::Roe<NetworkTuning> NetworkTuning::fromConfig(const pp::common::Object &config) {
  NetworkTuning t;
  if (!config.contains("network")) {
    return t;
  }
  const pp::common::Object *section = config.getObject("network");
  if (!section) {
    return pp::Error("field 'network' must be an object");
  }
  const pp::common::Object &jd = *section;
  const std::string s = "network";
  if (auto r = checkKnownKeys(jd, s, {"rpcTimeoutMs", "requestQueueCapacity", "startupSyncTimeoutMs", "amp"}); !r) {
    return r.error();
  }
  for (auto r : {readMs(jd, s, "rpcTimeoutMs", t.rpcTimeout),
                 readUInt(jd, s, "requestQueueCapacity", t.requestQueueCapacity),
                 readMs(jd, s, "startupSyncTimeoutMs", t.startupSyncTimeout)}) {
    if (!r) {
      return r.error();
    }
  }
  if (jd.contains("amp")) {
    const pp::common::Object *amp = jd.getObject("amp");
    if (!amp) {
      return pp::Error("field 'network.amp' must be an object");
    }
    if (auto r = readAmp(*amp, t); !r) {
      return r.error();
    }
  }
  if (auto r = t.validate(); !r) {
    return r.error();
  }
  return t;
}

pp::Roe<void> NetworkTuning::validate() const {
  if (rpcTimeout < std::chrono::seconds(1)) {
    return pp::Error("network.rpcTimeoutMs must be at least 1000");
  }
  if (requestQueueCapacity == 0) {
    return pp::Error("network.requestQueueCapacity must be positive");
  }
  if (startupSyncTimeout < rpcTimeout) {
    return pp::Error("network.startupSyncTimeoutMs must be at least network.rpcTimeoutMs");
  }
  pp::amp::PeerLinkConfig link;
  applyTo(link);
  if (auto r = pp::amp::ValidatePeerLinkConfig(link); !r) {
    return pp::Error("network.amp: " + r.error().message);
  }
  return {};
}

} // namespace network
} // namespace pp
