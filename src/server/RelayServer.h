#ifndef PP_LEDGER_RELAY_SERVER_H
#define PP_LEDGER_RELAY_SERVER_H

#include "../client/Client.h"
#include "FollowerServer.h"
#include "Relay.h"
#include "UpstreamSignedTip.h"
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

class RelayServer : public FollowerServer {
public:
  RelayServer();
  ~RelayServer() override = default;

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

protected:
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "RelayServer"; }
  void fillHealth(Client::NodeHealth &health) const override;

  /**
   * Called before service thread starts - initializes beacon and network
   */
  Service::Roe<void> onStart() override;

  void onSignedTip(const Client::SignedTip &tip) override { noteSignedTip(tip); }
  /** The held signed tip may no longer verify (a new certificate's cutover, equivocation evidence). */
  void onTrustChanged() override { dropHeldTipIfInvalid(); }
  void onSynced() override { startBeaconStateRefresh(); }
  /** A relay holds every account: it refuses what the beacon would, before forwarding it. */
  bool checksTransactions() const override { return true; }

private:
  constexpr static const char *FILE_LOG = "relay.log";
  constexpr static const char *FILE_SIGNATURE = ".signature";
  constexpr static const char *DIR_DATA = "data";
  void initHandlers();
  void dTxPull(const Client::Request &request, const RequestQueue::Reply &reply);
  /** Refresh registry version / network id from the beacon, without blocking. */
  void startBeaconStateRefresh();

  Client::BeaconState buildStateResponse() const;
  /** At start, once the relay has loaded: add the certificates' dial hints, rank and wait for it. */
  void rankUpstreamsAtStart();

  /** Serves BLOCK_WAIT: downstreams learn of each block once this relay has synced it. */
  std::optional<uint64_t> blockWaitTip() const override { return relay_.getNextBlockId(); }
  std::optional<Client::SignedTip> latestSignedTip() const override { return upstreamTip_.latest(); }
  /** Keep a signed tip heard from upstream (STATUS, BLOCK_WAIT, BLOCK_ADD and REGISTER replies). */
  void noteSignedTip(const std::optional<Client::SignedTip> &tip);
  /** Stop serving the held signed tip once it no longer verifies (a new certificate's cutover). */
  void dropHeldTipIfInvalid();
  /** True when `reply` carries a signed tip, valid here, for exactly `block`. */
  bool coversBlock(const Client::TipReply &reply, const Ledger::ChainNode &block) const;

  // Deferred: reply after the upstream's, off the server thread's back
  void dBlockAdd(const Client::Request &request, const RequestQueue::Reply &reply);
  void dRegister(const Client::Request &request, const RequestQueue::Reply &reply);
  void dMinerList(const Client::Request &request, const RequestQueue::Reply &reply);

  // Getters
  Roe<std::string> hStatus(const Client::Request &request);

  Relay relay_;
  bool beaconStateRefreshInFlight_{false};

  uint64_t registryVersion_{0};
  std::string networkId_;
  /** The beacon's newest signed tip, passed on unchanged. */
  UpstreamSignedTip upstreamTip_;
};

} // namespace pp

#endif // PP_LEDGER_RELAY_SERVER_H
