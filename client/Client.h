#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../ledger/Ledger.h"
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

  // Request types
  static constexpr const uint32_t T_REQ_JSON = 1;

  static constexpr const uint32_t T_REQ_BLOCK_GET = 1001;
  static constexpr const uint32_t T_REQ_BLOCK_ADD = 1002;

  static constexpr const uint32_t T_REQ_TRANSACTION_ADD = 2001;

  // Error codes
  static constexpr const uint16_t E_NOT_CONNECTED = 1;
  static constexpr const uint16_t E_INVALID_RESPONSE = 2;
  static constexpr const uint16_t E_SERVER_ERROR = 3;
  static constexpr const uint16_t E_PARSE_ERROR = 4;
  static constexpr const uint16_t E_REQUEST_FAILED = 5;

  // Get human-friendly error message for an error code
  static std::string getErrorMessage(uint16_t errorCode);

  struct Request {
    static constexpr const uint32_t VERSION = 1;

    uint32_t version{ VERSION };
    uint32_t type{ 0 };
    std::string payload;

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & version & type & payload;
    }
  };

  struct Response {
    static constexpr const uint32_t VERSION = 1;
    uint32_t version{ VERSION };
    uint16_t errorCode{ 0 };
    std::string payload;

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & version & errorCode & payload;
    }

    bool isError() const { return errorCode != 0; }
  };

  // Response data structures
  struct ServerInfo {
    uint64_t nextBlockId{ 0 };
    uint64_t currentSlot{ 0 };
    uint64_t currentEpoch{ 0 };
  };

  struct MinerStatus {
    uint64_t minerId{ 0 };
    uint64_t stake{ 0 };
    uint64_t nextBlockId{ 0 };
    uint64_t currentSlot{ 0 };
    uint64_t currentEpoch{ 0 };
    uint64_t pendingTransactions{ 0 };
    bool isSlotLeader{ false };
  };

  /** Beacon status: checkpoint, block, slot, epoch, timestamp and stakeholders (single round-trip). */
  struct BeaconState {
    uint64_t checkpointId{ 0 };
    uint64_t nextBlockId { 0 };
    uint64_t currentSlot { 0 };
    uint64_t currentEpoch { 0 };
    int64_t currentTimestamp { 0 };  /**< Unix time in seconds (server's view of now) */
    std::vector<consensus::Stakeholder> stakeholders;
  };

  Client();
  ~Client();

  Roe<void> setEndpoint(const std::string& endpoint);
  void setEndpoint(const network::TcpEndpoint &endpoint);

  Roe<uint64_t> fetchSlotLeader(uint64_t slot);

  Roe<BeaconState> fetchBeaconState();
  Roe<MinerStatus> fetchMinerStatus();
  Roe<std::vector<consensus::Stakeholder>> fetchStakeholders();
  Roe<Ledger::ChainNode> fetchBlock(uint64_t blockId);

  Roe<void> addTransaction(const nlohmann::json &transaction);
  Roe<bool> addBlock(const Ledger::ChainNode& block);
  Roe<bool> produceBlock();

private:
  // Helper to send JSON request and receive JSON response
  Roe<nlohmann::json> sendRequest(const nlohmann::json &request);

  // Helper to send binary request (type + payload) and receive raw response
  Roe<std::string> sendBinaryRequest(uint32_t type, const std::string &payload);

  bool connected_{false};
  network::TcpEndpoint endpoint_;
};

inline std::ostream& operator<<(std::ostream& os, const Client::Request& req) {
  os << "Request{version=" << req.version << ", type=" << req.type << ", payload=" << req.payload.size() << " bytes}";
  return os;
}

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
