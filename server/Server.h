#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "../interface/Block.hpp"
#include "../ledger/BlockChain.h"
#include "../network/FetchClient.h"
#include "../network/FetchServer.h"
#include "Ledger.h"
#include "ResultOrError.hpp"
#include "Service.h"
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <vector>

// Forward declarations for network classes
namespace pp {

class Server : public Service {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct NetworkConfig {
    bool enableP2P = false;
    std::string nodeId;
    std::vector<std::string> bootstrapPeers; // host:port format
    std::string listenAddr = "0.0.0.0";
    uint16_t p2pPort = 9000;
    uint16_t maxPeers = 50;
  };

  Server();
  ~Server() override = default;

  // Server lifecycle
  bool start(const std::string &dataDir);

protected:
  /**
   * Service thread main loop - runs consensus
   */
  void run() override;

  /**
   * Called before service thread starts - starts FetchServer
   */
  bool onStart() override;

  /**
   * Called after service thread stops - stops FetchServer
   */
  void onStop() override;

private:
  // Configuration loading
  Roe<void> loadConfig(const std::string &configPath);

  // Storage initialization
  Roe<void> initStorage(const std::string &dataDir);

  // Network management
  size_t getPeerCount() const;
  std::vector<std::string> getConnectedPeers() const;

  // Consensus management
  void registerStakeholder(const std::string &id, uint64_t stake);
  void setSlotDuration(uint64_t seconds);

  // Transaction management
  void submitTransaction(const std::string &transaction);

  // State queries
  uint64_t getCurrentSlot() const;
  uint64_t getCurrentEpoch() const;
  size_t getBlockCount() const;
  Roe<int64_t> getBalance(const std::string &walletId) const;

  std::string handleIncomingRequest(const std::string &request);
  std::string handleClientRequest(const Client::Request &request);
  void broadcastBlock(std::shared_ptr<iii::Block> block);

  // Client request handlers
  Roe<std::string> handleReqInfo();
  Roe<std::string> handleReqQueryWallet(const std::string &requestData);
  Roe<std::string> handleReqAddTransaction(const std::string &requestData);
  Roe<std::string> handleReqValidators();
  Roe<std::string> handleReqBlocks(const std::string &requestData);
  void requestBlocksFromPeers(uint64_t fromIndex);
  Roe<std::vector<std::shared_ptr<iii::Block>>>
  fetchBlocksFromPeer(const std::string &hostPort, uint64_t fromIndex);

  // Helper to parse host:port string
  static bool parseHostPort(const std::string &hostPort, std::string &host,
                            uint16_t &port);

  bool shouldProduceBlock() const;
  Roe<void> produceBlock();
  Roe<void> syncState();
  Roe<std::shared_ptr<iii::Block>> createBlockFromTransactions();
  Roe<void> addBlockToLedger(std::shared_ptr<iii::Block> block);

  // Chain switching support
  Roe<std::unique_ptr<BlockChain>> buildCandidateChainFromBlocks(
      const std::vector<std::shared_ptr<iii::Block>> &blocks) const;
  Roe<void> switchToChain(std::unique_ptr<BlockChain> candidateChain);

  // Consensus and ledger management
  Ledger ledger_;
  consensus::Ouroboros consensus_;

  // Server state
  int port_{0};

  // Client server (listens on main port)
  network::FetchServer sFetch_;
  network::FetchClient cFetch_;

  mutable std::mutex peersMutex_;
  std::set<std::string> connectedPeers_; // Set of host:port strings
  NetworkConfig networkConfig_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
