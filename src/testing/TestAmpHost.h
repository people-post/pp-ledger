#pragma once

#include "ledger/network/HostedAmpRuntime.h"
#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "amp/L1/Clock.h"
#include "amp/L1/MemoryDatagramIo.h"
#include "amp/link/AdpMultiaddr.h"
#include "amp/link/AmpStack.h"
#include "ledger/lib/common/Utilities.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pp::test {

/** A fresh ML-DSA identity (and its peer id) for an Amp runtime. */
inline network::LedgerAmpConfig freshAmpIdentity() {
  return network::LedgerAmpConfigFromPrivateKey(utl::mlDsaGenerate().value().privateKey, 0).value();
}

/**
 * pp-node's part in tests (platform-integration): an Amp stack on an in-memory hub that it owns and
 * drives from a thread of its own, advertising a protocol of its own.
 */
struct TestAmpHost {
  static constexpr const char *kProtocol = "/pp-mesh/test/1.0.0";

  std::unique_ptr<pp::amp::AmpStack> stack;
  std::string listen;
  std::thread pump;
  std::atomic<bool> stopping{false};

  void start(const std::shared_ptr<pp::adp::MemoryDatagramHub> &hub, const pp::adp::IpEndpoint &address) {
    auto config = freshAmpIdentity();
    pp::amp::AmpStack::Config stackConfig;
    stackConfig.identity = config.identity;
    stackConfig.local_peer_id = config.local_peer_id;
    stackConfig.link_config = config.link_config;
    auto io = std::make_shared<pp::adp::MemoryDatagramIo>(hub, address);
    auto created = pp::amp::AmpStack::Create(io, std::make_shared<pp::adp::WallClock>(), stackConfig);
    stack = std::move(*created);
    stack->Start();
    stack->GetEndpoint().SetAcceptEnabled(true);
    stack->Links().SetRefuseUnhandledOpens(true);
    listen = pp::amp::FormatAdpMultiaddr(io->LocalEndpoint(), stack->LocalPeerId()).value();
    stack->Links().SetLocalListenMultiaddrs({listen});
    stack->Links().SetAdvertisedProtocols({kProtocol});
    pump = std::thread([this]() {
      while (!stopping) {
        stack->Runtime().Drive();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  network::HostedAmpRuntime::Host forLedger() {
    return {&stack->Runtime(), [address = listen]() { return address; }};
  }
  ~TestAmpHost() {
    stopping = true;
    if (pump.joinable()) {
      pump.join();
    }
    if (stack) {
      stack->Stop();
    }
  }
};

} // namespace pp::test
