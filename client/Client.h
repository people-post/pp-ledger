#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"
#include "../ledger/Ledger.h"
#include "../network/FetchClient.h"
#include "../network/Types.hpp"
#include "../consensus/Types.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pp {

class Client : public Module {
public:
  struct Wallet {
    std::map<uint64_t, int64_t> mBalances; // tokenId -> balance
    std::vector<std::string> publicKeys;
    uint8_t minSignatures{ 0 };

    bool operator==(const Wallet& other) const {
      return mBalances == other.mBalances &&
             publicKeys == other.publicKeys &&
             minSignatures == other.minSignatures;
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & mBalances & publicKeys & minSignatures;
    }

    nlohmann::json toJson() const;
  };

  struct UserAccount {
    constexpr static const uint32_t VERSION = 1;

    Wallet wallet;
    std::string meta;

    bool operator==(const UserAccount& other) const {
      return wallet == other.wallet && meta == other.meta;
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & wallet & meta;
    }

    std::string ltsToString() const;
    bool ltsFromString(const std::string& str);
    nlohmann::json toJson() const;
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Default connection settings
  static constexpr const char *DEFAULT_HOST = "localhost";
  static constexpr const uint16_t DEFAULT_BEACON_PORT = 8517;
  static constexpr const uint16_t DEFAULT_MINER_PORT = 8518;

  // Request timeouts (generous enough for beacon under load, e.g. test-checkpoint-cycles)
  /** Timeout for fast, lightweight requests (status, timestamp, registration). */
  static constexpr std::chrono::milliseconds TIMEOUT_FAST{15000};
  /** Timeout for data-retrieval or data-submission requests (blocks, transactions, accounts). */
  static constexpr std::chrono::milliseconds TIMEOUT_DATA{30000};

  // Request types
  static constexpr const uint32_t T_REQ_STATUS = 1;
  static constexpr const uint32_t T_REQ_REGISTER = 2;
  static constexpr const uint32_t T_REQ_MINER_LIST = 3;
  /** Request precise server timestamp in ms since epoch for time calibration. */
  static constexpr const uint32_t T_REQ_CALIBRATION = 4;

  static constexpr const uint32_t T_REQ_BLOCK_GET = 1001;
  static constexpr const uint32_t T_REQ_BLOCK_ADD = 1002;

  static constexpr const uint32_t T_REQ_ACCOUNT_GET = 2001;

  static constexpr const uint32_t T_REQ_TX_GET_BY_WALLET = 3001;
  static constexpr const uint32_t T_REQ_TX_ADD = 3002;

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
  struct MinerInfo {
    uint64_t id{ 0 };
    int64_t tLastMessage{ 0 };
    std::string endpoint;

    nlohmann::json ltsToJson() const;
    Roe<bool> ltsFromJson(const nlohmann::json& json);
  };

  struct MinerStatus {
    uint64_t minerId{ 0 };
    uint64_t stake{ 0 };
    uint64_t nextBlockId{ 0 };
    uint64_t currentSlot{ 0 };
    uint64_t currentEpoch{ 0 };
    uint64_t pendingTransactions{ 0 };
    uint64_t nStakeholders{ 0 };
    bool isSlotLeader{ false };

    nlohmann::json ltsToJson() const;
    Roe<bool> ltsFromJson(const nlohmann::json &json);
  };

  /** Beacon status: checkpoint, block, slot, epoch, timestamp and stakeholders (single round-trip). */
  struct BeaconState {
    int64_t currentTimestamp { 0 };  /**< Unix time in seconds (server's view of now) */
    uint64_t lastCheckpointId{ 0 };
    uint64_t checkpointId{ 0 };
    uint64_t nextBlockId { 0 };
    uint64_t currentSlot { 0 };
    uint64_t currentEpoch { 0 };
    uint64_t nStakeholders { 0 };

    nlohmann::json ltsToJson() const;
    Roe<bool> ltsFromJson(const nlohmann::json &json);
  };

  struct TxGetByWalletRequest {
    uint64_t walletId{ 0 };
    uint64_t beforeBlockId{ 0 };

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & walletId & beforeBlockId;
    }
  };

  struct TxGetByWalletResponse {
    std::vector<Ledger::SignedData<Ledger::Transaction>> transactions;
    uint64_t nextBlockId{ 0 };

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & transactions & nextBlockId;
    }

    nlohmann::json toJson() const;
  };

  struct CalibrationResponse {
    int64_t msTimestamp{ 0 };
    uint64_t nextBlockId{ 0 };
    
    template <typename Archive>
    void serialize(Archive &ar) {
      ar & msTimestamp & nextBlockId;
    }

    nlohmann::json toJson() const;
  };

  Client();
  ~Client() override;

  Roe<void> setEndpoint(const std::string& endpoint);
  void setEndpoint(const network::TcpEndpoint &endpoint);

  Roe<BeaconState> fetchBeaconState();
  /** Fetch server's current time in milliseconds since Unix epoch (for calibration). */
  Roe<CalibrationResponse> fetchCalibration();
  Roe<BeaconState> registerMinerServer(const MinerInfo &minerInfo);
  Roe<std::vector<MinerInfo>> fetchMinerList();
  Roe<MinerStatus> fetchMinerStatus();
  Roe<Ledger::ChainNode> fetchBlock(uint64_t blockId);
  Roe<UserAccount> fetchUserAccount(const uint64_t accountId);
  Roe<TxGetByWalletResponse> fetchTransactionsByWallet(const TxGetByWalletRequest &request);

  Roe<void> addTransaction(const Ledger::SignedData<Ledger::Transaction> &signedTx);
  Roe<bool> addBlock(const Ledger::ChainNode& block);

private:
  Roe<std::string> sendRequest(uint32_t type, const std::string &payload,
                               std::chrono::milliseconds timeout = TIMEOUT_FAST);

  bool connected_{false};
  network::TcpEndpoint endpoint_;
  network::FetchClient fetchClient_;
};

std::ostream& operator<<(std::ostream& os, const Client::Request& req);
std::ostream& operator<<(std::ostream& os, const Client::Wallet& wallet);
std::ostream& operator<<(std::ostream& os, const Client::UserAccount& account);

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
