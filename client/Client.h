#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../network/Types.hpp"
#include "../consensus/Types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pp {

class Client : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Default connection settings
  static constexpr const char *DEFAULT_HOST = "localhost";
  static constexpr const uint16_t DEFAULT_BEACON_PORT = 8517;
  static constexpr const uint16_t DEFAULT_MINER_PORT = 8518;

  // Error codes
  static constexpr const uint16_t E_NOT_CONNECTED = 1;
  static constexpr const uint16_t E_INVALID_RESPONSE = 2;
  static constexpr const uint16_t E_SERVER_ERROR = 3;
  static constexpr const uint16_t E_PARSE_ERROR = 4;
  static constexpr const uint16_t E_REQUEST_FAILED = 5;

  // Get human-friendly error message for an error code
  static std::string getErrorMessage(uint16_t errorCode);

  // Response data structures
  struct BlockInfo {
    uint64_t index;
    int64_t timestamp;
    std::string data;
    std::string previousHash;
    std::string hash;
    uint64_t slot;
    std::string slotLeader;
  };

  struct ServerInfo {
    uint64_t currentBlockId;
    uint64_t currentSlot;
    uint64_t currentEpoch;
  };

  struct MinerStatus {
    std::string minerId;
    uint64_t stake;
    uint64_t currentBlockId;
    uint64_t currentSlot;
    uint64_t currentEpoch;
    uint64_t pendingTransactions;
    bool isSlotLeader;
  };

  /** Beacon status: checkpoint, block, slot, epoch, timestamp and stakeholders (single round-trip). */
  struct BeaconState {
    uint64_t checkpointId;
    uint64_t blockId;
    uint64_t currentSlot;
    uint64_t currentEpoch;
    int64_t currentTimestamp;  /**< Unix time in seconds (server's view of now) */
    std::vector<consensus::Stakeholder> stakeholders;
  };

  Client();
  ~Client();

  Roe<void> setEndpoint(const std::string& endpoint);
  void setEndpoint(const network::TcpEndpoint &endpoint);

  // BeaconServer API - Block operations
  Roe<BlockInfo> fetchBlock(uint64_t blockId);
  Roe<uint64_t> fetchCurrentBlockId();
  Roe<bool> addBlock(const BlockInfo &block);

  // BeaconServer API - Checkpoint operations
  Roe<uint64_t> fetchCurrentCheckpointId();

  // BeaconServer API - Stakeholder operations
  Roe<std::vector<consensus::Stakeholder>> fetchStakeholders();

  // BeaconServer API - Combined state (checkpoint id, block id, stakeholders) in one call
  Roe<BeaconState> fetchBeaconState();

  // BeaconServer API - Consensus queries
  Roe<uint64_t> fetchCurrentSlot();
  Roe<uint64_t> fetchCurrentEpoch();
  Roe<std::string> fetchSlotLeader(uint64_t slot);

  // MinerServer API - Transaction operations
  Roe<bool> addTransaction(const nlohmann::json &transaction);
  Roe<uint64_t> fetchPendingTransactionCount();

  // MinerServer API - Mining operations
  Roe<bool> produceBlock();
  Roe<bool> fetchIsSlotLeader();

  // MinerServer API - Status
  Roe<MinerStatus> fetchMinerStatus();

private:
  // Helper to send JSON request and receive JSON response
  Roe<nlohmann::json> sendRequest(const nlohmann::json &request);

  bool connected_{false};
  network::TcpEndpoint endpoint_;
};

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
