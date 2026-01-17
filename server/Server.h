#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../client/Client.h"
#include "../consensus/Ouroboros.h"
#include "../interface/Block.hpp"
#include "../ledger/BlockChain.h"
#include "../network/FetchClient.h"
#include "../network/FetchServer.h"
#include "Agent.h"
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
    std::string host;
    uint16_t port{ 0 };

    bool enableP2P = false;
    std::string nodeId;
    std::vector<std::string> bootstrapPeers; // host:port format
    std::string listenAddr = "0.0.0.0";
    uint16_t p2pPort = 9000;
    uint16_t maxPeers = 50;
  };

  struct Config {
    NetworkConfig network;
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
  bool shouldProduceBlock() const;

  // Helper to parse host:port string
  static bool parseHostPort(const std::string &hostPort, std::string &host,
                            uint16_t &port);

  Roe<void> loadConfig(const std::string &configPath);
  Roe<void> initAgent(const std::string &dataDir);
  Roe<std::unique_ptr<BlockChain>> buildCandidateChainFromBlocks(
      const std::vector<std::shared_ptr<iii::Block>> &blocks) const;

  // Transaction management
  void addTransaction(const std::string &transaction);

  Roe<void> produceBlock();

  // State synchronization
  Roe<void> syncState();
  void requestBlocksFromPeers(uint64_t fromIndex);
  Roe<std::vector<std::shared_ptr<iii::Block>>>
  fetchBlocksFromPeer(const std::string &hostPort, uint64_t fromIndex);
  void broadcastBlock(std::shared_ptr<iii::Block> block);

  // Chain switching
  Roe<void> switchToChain(std::unique_ptr<BlockChain> candidateChain);

  std::string handleIncomingRequest(const std::string &request);
  std::string handleClientRequest(const Client::Request &request);

  // Client request handlers
  Roe<std::string> handleReqInfo();
  Roe<std::string> handleReqQueryWallet(const std::string &requestData);
  Roe<std::string> handleReqAddTransaction(const std::string &requestData);
  Roe<std::string> handleReqValidators();
  Roe<std::string> handleReqBlocks(const std::string &requestData);


  // Consensus and agent management
  Agent agent_;
  consensus::Ouroboros consensus_;

  // Client server (listens on main port)
  network::FetchServer sFetch_;
  network::FetchClient cFetch_;

  Config config_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
