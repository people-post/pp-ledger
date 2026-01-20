#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
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

  // Request types
  // Client to Server
  static constexpr const uint16_t T_REQ_INFO =
      1001; // Request information about the server
  static constexpr const uint16_t T_REQ_QUERY_WALLET =
      1002; // Request information about wallet
  static constexpr const uint16_t T_REQ_ADD_TRANSACTION =
      1003; // Request add transaction

  // Server to Beacon
  static constexpr const uint16_t T_REQ_BEACON_VALIDATORS =
      2001; // Request validators from beacon

  // Server to Server
  static constexpr const uint16_t T_REQ_BLOCKS =
      3001; // Request blocks from the server
  static constexpr const uint16_t T_REQ_PEER_VALIDATORS =
      3002; // Request validators from peer server

  static constexpr const uint16_t VERSION = 1; // Version of the protocol

  // Default connection settings
  static constexpr const char *DEFAULT_HOST = "localhost";
  static constexpr const uint16_t DEFAULT_PORT = 8517;

  // Error codes
  static constexpr const uint16_t E_VERSION = 1;           // Version mismatch
  static constexpr const uint16_t E_INVALID_REQUEST = 2;   // Invalid request
  static constexpr const uint16_t E_INVALID_RESPONSE = 3;  // Invalid response
  static constexpr const uint16_t E_INVALID_DATA = 4;      // Invalid data
  static constexpr const uint16_t E_INVALID_SIGNATURE = 5; // Invalid signature
  static constexpr const uint16_t E_INVALID_TIMESTAMP = 6; // Invalid timestamp
  static constexpr const uint16_t E_INVALID_NONCE = 7;     // Invalid nonce
  static constexpr const uint16_t E_INVALID_HASH = 8;      // Invalid hash
  static constexpr const uint16_t E_INVALID_BLOCK = 9;     // Invalid block
  static constexpr const uint16_t E_INVALID_TRANSACTION =
      10; // Invalid transaction
  static constexpr const uint16_t E_INVALID_VALIDATOR = 11; // Invalid validator
  static constexpr const uint16_t E_INVALID_BLOCKCHAIN =
      12;                                                 // Invalid blockchain
  static constexpr const uint16_t E_INVALID_LEDGER = 13;  // Invalid ledger
  static constexpr const uint16_t E_INVALID_WALLET = 14;  // Invalid wallet
  static constexpr const uint16_t E_INVALID_ADDRESS = 15; // Invalid address

  // Get human-friendly error message for an error code
  static std::string getErrorMessage(uint16_t errorCode);

  struct Request {
    uint16_t version;
    uint16_t type;
    std::string data;

    template <typename Archive> void serialize(Archive &ar) {
      ar &version &type &data;
    }
  };

  struct Response {
    uint16_t version;
    uint16_t errorCode;
    uint16_t type;
    std::string data;

    template <typename Archive> void serialize(Archive &ar) {
      ar &version &errorCode &type &data;
    }
  };

  struct ReqWalletInfo {
    std::string walletId;

    template <typename Archive> void serialize(Archive &ar) { ar &walletId; }
  };

  struct ReqAddTransaction {
    std::string transaction;

    template <typename Archive> void serialize(Archive &ar) { ar &transaction; }
  };

  struct ReqValidators {
    std::string validators;

    template <typename Archive> void serialize(Archive &ar) { ar &validators; }
  };

  struct ReqBlocks {
    uint64_t fromIndex;
    uint64_t count;

    template <typename Archive> void serialize(Archive &ar) {
      ar &fromIndex &count;
    }
  };

  struct RespWalletInfo {
    std::string walletId;
    int64_t balance;

    template <typename Archive> void serialize(Archive &ar) {
      ar &walletId &balance;
    }
  };

  struct RespAddTransaction {
    std::string transaction;

    template <typename Archive> void serialize(Archive &ar) { ar &transaction; }
  };

  struct BlockInfo {
    uint64_t index;
    int64_t timestamp;
    std::string data;
    std::string previousHash;
    std::string hash;
    uint64_t slot;
    std::string slotLeader;

    template <typename Archive> void serialize(Archive &ar) {
      ar &index &timestamp &data &previousHash &hash &slot &slotLeader;
    }
  };

  struct RespBlocks {
    std::vector<BlockInfo> blocks;

    template <typename Archive> void serialize(Archive &ar) { ar &blocks; }
  };

  struct ValidatorInfo {
    std::string id;
    uint64_t stake;

    template <typename Archive> void serialize(Archive &ar) { ar &id &stake; }
  };

  struct RespValidators {
    std::vector<ValidatorInfo> validators;

    template <typename Archive> void serialize(Archive &ar) { ar &validators; }
  };

  struct RespInfo {
    uint64_t blockCount;
    uint64_t currentSlot;
    uint64_t currentEpoch;
    uint64_t pendingTransactions;

    template <typename Archive> void serialize(Archive &ar) {
      ar &blockCount &currentSlot &currentEpoch &pendingTransactions;
    }
  };

  Client();
  ~Client();

  bool init(const std::string &address = DEFAULT_HOST, int port = DEFAULT_PORT);

  Roe<RespInfo> getInfo();
  Roe<RespWalletInfo> getWalletInfo(const std::string &walletId);
  Roe<RespAddTransaction> addTransaction(const std::string &transaction);
  Roe<RespValidators> getValidators();
  Roe<RespBlocks> getBlocks(uint64_t fromIndex, uint64_t count);

  void disconnect();
  bool isConnected() const;

private:
  // Helper to send request and receive response
  Roe<Response> sendRequest(const Request &request);

  bool connected_;
  std::string address_;
  int port_;
};

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
