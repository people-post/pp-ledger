#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "ILedgerTransport.h"
#include "InProcessLedgerTransport.h"
#include "../network/LedgerAmpRuntime.h"
#include "../network/NetworkTuning.h"
#include "amp/link/PeerLinkManager.h"
#include "lib/common/Meta.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"
#include "../ledger/Ledger.h"
#include "../consensus/Types.hpp"

#include <chrono>
#include <memory>
#include <optional>

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
    uint8_t keyType{ 0 };  // Crypto::TK_ML_DSA_65 = 1; use Crypto::isSupported() to check

    bool operator==(const Wallet& other) const {
      return mBalances == other.mBalances &&
             publicKeys == other.publicKeys &&
             minSignatures == other.minSignatures &&
             keyType == other.keyType;
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & mBalances & publicKeys & minSignatures & keyType;
    }

    pp::common::Meta ltsToMeta() const;
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
    pp::common::Meta ltsToMeta() const;
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Default connection settings
  static constexpr const char *DEFAULT_HOST = "localhost";
  static constexpr const uint16_t DEFAULT_BEACON_PORT = 8517;
  static constexpr const uint16_t DEFAULT_MINER_PORT = 8518;


  // Request types
  static constexpr const uint32_t T_REQ_STATUS = 1;
  static constexpr const uint32_t T_REQ_REGISTER = 2;
  static constexpr const uint32_t T_REQ_MINER_LIST = 3;
  /** Request precise server timestamp in ms since epoch for time calibration. */
  static constexpr const uint32_t T_REQ_CALIBRATION = 4;

  static constexpr const uint32_t T_REQ_BLOCK_GET = 1001;
  static constexpr const uint32_t T_REQ_BLOCK_ADD = 1002;
  /**
   * Downstream → upstream: payload binaryPack(uint64 knownNextBlockId). The
   * upstream replies binaryPack(uint64 its nextBlockId) once that passes the
   * known one, or after a hold (NetworkTuning::blockWaitHold) with it unchanged.
   */
  static constexpr const uint32_t T_REQ_BLOCK_WAIT = 1003;

  static constexpr const uint32_t T_REQ_ACCOUNT_GET = 2001;
  // Reserved for name-directory (docs/product/NAME_DIRECTORY.md); not wired yet.
  static constexpr const uint32_t T_REQ_DOMAIN_GET = 2101;
  static constexpr const uint32_t T_REQ_NAME_GET = 2102;
  static constexpr const uint32_t T_REQ_NAME_GET_BY_WALLET = 2103;

  static constexpr const uint32_t T_REQ_TX_GET_BY_WALLET = 3001;
  static constexpr const uint32_t T_REQ_TX_ADD = 3002;
  static constexpr const uint32_t T_REQ_TX_GET_BY_INDEX = 3003;
  // 3004 retired (TX_FORWARD: miner→miner forward, replaced by TX_PULL).
  /**
   * Slot leader → upstream: payload binaryPack(uint64 slot); reply
   * binaryPack(vector<Ledger::Record>) — pending transactions from the beacon's
   * pool (TxPool). Relays pass it through.
   */
  static constexpr const uint32_t T_REQ_TX_PULL = 3005;

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

  /**
   * The `type` of a packed Request without unpacking its payload (it follows
   * `version`, both big-endian u32). nullopt when the body is too short.
   */
  static std::optional<uint32_t> peekRequestType(const std::string &body) {
    if (body.size() < 8) {
      return std::nullopt;
    }
    uint32_t type = 0;
    for (size_t i = 4; i < 8; ++i) {
      type = (type << 8) | static_cast<uint8_t>(body[i]);
    }
    return type;
  }

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
  /**
   * A registered miner. No network address: miners are reached only through
   * their relays, and transactions reach the leader by TX_PULL.
   */
  struct MinerInfo {
    uint64_t id{ 0 };
    /** Set by the beacon when it records the registration (its clock). */
    int64_t tLastMessage{ 0 };
    /** Signer's clock (Unix seconds) when signed; the beacon refuses stale or replayed records. */
    int64_t issuedAt{ 0 };
    /** Miner account's signatures over signingMessage() (as for its transactions). */
    std::vector<std::string> signatures;

    /** Domain-separated bytes the miner signs: network, id, issuedAt. */
    std::string signingMessage(const std::string &networkId) const;

    pp::common::Meta ltsToMeta() const;
    Roe<bool> ltsFromMeta(const pp::common::Meta &meta);
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

    pp::common::Meta ltsToMeta() const;
    Roe<bool> ltsFromMeta(const pp::common::Meta &meta);
  };

  /** Beacon status: checkpoint, block, slot, epoch, timestamp and stakeholders (single round-trip). */
  struct BeaconState {
    int64_t currentTimestamp { 0 };  /**< Unix time in seconds (server's view of now) */
    uint64_t checkpointId{ 0 };
    uint64_t nextBlockId { 0 };
    uint64_t currentSlot { 0 };
    uint64_t currentEpoch { 0 };
    uint64_t nStakeholders { 0 };
    /** Optional STATUS v2 fields (see docs/architecture/LEDGER_TOPOLOGY.md). */
    std::string networkId;
    std::string headHash;
    uint64_t registryVersion{ 0 };

    pp::common::Meta ltsToMeta() const;
    Roe<bool> ltsFromMeta(const pp::common::Meta &meta);
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
    std::vector<Ledger::Record> transactions;
    uint64_t nextBlockId{ 0 };

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & transactions & nextBlockId;
    }

    pp::common::Meta ltsToMeta() const;
  };

  struct TxGetByIndexRequest {
    uint64_t txIndex{ 0 };

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & txIndex;
    }
  };

  struct CalibrationResponse {
    int64_t msTimestamp{ 0 };
    uint64_t nextBlockId{ 0 };
    
    template <typename Archive>
    void serialize(Archive &ar) {
      ar & msTimestamp & nextBlockId;
    }

    pp::common::Meta ltsToMeta() const;
  };

  Client();
  ~Client() override;

  Roe<void> setAmpPeer(const std::string& peer_key, const std::string& multiaddr);

  /** Attach AMP transport driven by `runtime`'s pump thread (servers / CLI / pp-http). */
  void attachAmpTransport(network::LedgerAmpRuntime& runtime, std::string default_peer_key = "remote");

  /** Replace the transport (e.g. in-process for embedded UI). */
  void setTransport(std::unique_ptr<ILedgerTransport> transport);

  /**
   * Wait for a light reply (status, calibration, registration); requests that
   * carry blocks, transactions or accounts wait twice this. Servers set it from
   * NetworkTuning::rpcTimeout.
   */
  void setRequestTimeout(std::chrono::milliseconds timeout) { requestTimeout_ = timeout; }

  ILedgerTransport *transport() { return transport_.get(); }
  const ILedgerTransport *transport() const { return transport_.get(); }

  Roe<BeaconState> fetchBeaconState();
  /** Fetch server's current time in milliseconds since Unix epoch (for calibration). */
  Roe<CalibrationResponse> fetchCalibration();
  Roe<BeaconState> registerMinerServer(const MinerInfo &minerInfo);
  Roe<std::vector<MinerInfo>> fetchMinerList();
  Roe<MinerStatus> fetchMinerStatus();
  Roe<Ledger::ChainNode> fetchBlock(uint64_t blockId);
  Roe<UserAccount> fetchUserAccount(const uint64_t accountId);
  Roe<TxGetByWalletResponse> fetchTransactionsByWallet(const TxGetByWalletRequest &request);
  Roe<Ledger::Record> fetchTransactionByIndex(const TxGetByIndexRequest &request);

  Roe<void> addTransaction(const Ledger::Record &record);
  Roe<bool> addBlock(const Ledger::ChainNode& block);

  /**
   * Async variants: start the call and return; `done` runs once with the
   * result, possibly on a transport thread (AMP io lane) — hop to your own
   * thread before touching state.
   */
  template <typename T> using Done = std::function<void(Roe<T>)>;
  void registerMinerServerAsync(const MinerInfo &minerInfo, Done<BeaconState> done);
  void fetchMinerListAsync(Done<std::vector<MinerInfo>> done);
  void addTransactionAsync(const Ledger::Record &record, Done<void> done);
  void addBlockAsync(const Ledger::ChainNode &block, Done<bool> done);
  /** TX_PULL: pending transactions for the leader of `slot`. */
  void pullTransactionsAsync(uint64_t slot, Done<std::vector<Ledger::Record>> done);
  void fetchCalibrationAsync(Done<CalibrationResponse> done);
  void fetchBlockAsync(uint64_t blockId, Done<Ledger::ChainNode> done);
  void fetchBeaconStateAsync(Done<BeaconState> done);
  /** BLOCK_WAIT: `done` gets the upstream's next block id once it passes `knownNextBlockId` (or unchanged after a hold). */
  void waitForBlockAsync(uint64_t knownNextBlockId, Done<uint64_t> done);

  /** True when the call failed before a server answered (worth retrying later). */
  static bool isTransportError(int32_t code) {
    return code == E_NOT_CONNECTED || code == E_REQUEST_FAILED;
  }

private:
  static Roe<BeaconState> parseBeaconState(const std::string &payload);
  static Roe<CalibrationResponse> parseCalibration(const std::string &payload);
  static Roe<Ledger::ChainNode> parseBlock(const std::string &payload);
  static Roe<std::vector<MinerInfo>> parseMinerList(const std::string &payload);
  std::string packRequest(uint32_t type, const std::string &payload);
  static Roe<std::string> parseResponse(const ILedgerTransport::Roe<std::string> &result);
  void sendRequestAsync(uint32_t type, const std::string &payload,
                        std::chrono::milliseconds timeout, Done<std::string> done);
  Roe<std::string> sendRequest(uint32_t type, const std::string &payload, std::chrono::milliseconds timeout);
  std::chrono::milliseconds fastTimeout() const { return requestTimeout_; }
  std::chrono::milliseconds dataTimeout() const { return network::NetworkTuning::dataTimeoutFor(requestTimeout_); }

  std::string amp_default_peer_key_{"remote"};
  std::unique_ptr<ILedgerTransport> transport_;
  std::chrono::milliseconds requestTimeout_{network::NetworkTuning::kDefaultRpcTimeout};
};

std::ostream& operator<<(std::ostream& os, const Client::Request& req);
std::ostream& operator<<(std::ostream& os, const Client::Wallet& wallet);
std::ostream& operator<<(std::ostream& os, const Client::UserAccount& account);

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
