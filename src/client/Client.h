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
  /**
   * Miner registration and liveness (docs/contracts/WIRE_SCHEMA.md).
   * Chain-owned: only T_MINER_REGISTER / T_MINER_REVOKE and block apply change it; account
   * creation starts it empty (genesis miners registered) and user updates / renewals must
   * carry it unchanged.
   */
  struct MinerState {
    constexpr static uint64_t kNever = UINT64_MAX;

    bool registered{ false };
    /** Slot of its last block (or of its registration); it is due once this is old enough. */
    uint64_t lastProducedSlot{ 0 };
    /** Consecutive slots it led while due and produced nothing. */
    uint32_t missedDue{ 0 };
    /** Epoch it was deregistered by the liveness rule (cool-down); kNever if never. */
    uint64_t autoDeregisteredEpoch{ kNever };

    bool operator==(const MinerState& other) const = default;

    template <typename Archive> void serialize(Archive &ar) {
      ar & registered & lastProducedSlot & missedDue & autoDeregisteredEpoch;
    }
  };

  struct Wallet {
    std::map<uint64_t, int64_t> mBalances; // tokenId -> balance
    std::vector<std::string> publicKeys;
    uint8_t minSignatures{ 0 };
    uint8_t keyType{ 0 };  // Crypto::TK_ML_DSA_65 = 1; use Crypto::isSupported() to check
    MinerState miner;

    bool operator==(const Wallet& other) const {
      return mBalances == other.mBalances &&
             publicKeys == other.publicKeys &&
             minSignatures == other.minSignatures &&
             keyType == other.keyType &&
             miner == other.miner;
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & mBalances & publicKeys & minSignatures & keyType & miner;
    }

    pp::common::Meta ltsToMeta() const;
  };

  struct UserAccount {
    constexpr static const uint32_t VERSION = 2;

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
   * upstream replies binaryPack(TipReply) once its nextBlockId passes the known
   * one, or after a hold (NetworkTuning::blockWaitHold) with it unchanged.
   */
  static constexpr const uint32_t T_REQ_BLOCK_WAIT = 1003;
  /**
   * Payload binaryPack(uint64 upToBlockId); reply binaryPack(AuthorityProof): the records that
   * changed the beacon authority from genesis up to that block. Served by nodes holding the chain
   * from genesis (beacon, relays).
   */
  static constexpr const uint32_t T_REQ_AUTHORITY_PROOF = 1004;
  /**
   * Downstream → upstream (beacon-authority ba6): payload Equivocation::ltsToString(). Each node
   * keeps the evidence if it verifies and halts on it; relays and miners pass it on up, so the
   * beacon halts too. Reply: a short text.
   */
  static constexpr const uint32_t T_REQ_EQUIVOCATION_ADD = 1005;

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

  /**
   * The beacon's signature over a block it committed (beacon-authority ba3): only the latest is
   * kept and served. One signature authenticates the hash chain below it; `generation` names the
   * beacon certificate whose key signed.
   */
  struct SignedTip {
    constexpr static const uint32_t VERSION = 1;

    std::string networkId;
    uint64_t generation{0};
    uint64_t height{0};    ///< block index
    std::string blockHash; ///< raw block hash
    std::string signature; ///< ML-DSA-65, by the beacon key, over signingMessage()

    /** Domain-separated bytes the beacon signs: network, generation, height, block hash. */
    std::string signingMessage() const;
    /** True when `signature` is `publicKey`'s over signingMessage(). */
    bool verify(const std::string &publicKey) const;

    bool operator==(const SignedTip &other) const {
      return networkId == other.networkId && generation == other.generation && height == other.height &&
             blockHash == other.blockHash && signature == other.signature;
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & networkId & generation & height & blockHash & signature;
    }

    /** Versioned bytes (disk, and the `signedTip` field of STATUS). */
    std::string ltsToString() const;
    bool ltsFromString(const std::string &str);
  };

  /**
   * Equivocation evidence (beacon-authority ba6): two signed tips of one network, generation and
   * height naming different blocks — proof that the generation's beacon key signed twice (stolen,
   * or two hosts running it). Signatures alone may differ for one block (ML-DSA signs with fresh
   * randomness), so only the block hash counts. Stored in canonical order: first.blockHash <
   * second.blockHash.
   */
  struct Equivocation {
    constexpr static const uint32_t VERSION = 1;

    SignedTip first;
    SignedTip second;

    /** Evidence from two tips, or none if they are not one network, generation and height with
     *  different blocks. Signatures are not checked here (verify()). */
    static std::optional<Equivocation> of(const SignedTip &a, const SignedTip &b);
    /** Same shape as of() requires, and both tips signed by `publicKey`. */
    bool verify(const std::string &publicKey) const;

    const std::string &networkId() const { return first.networkId; }
    uint64_t generation() const { return first.generation; }
    uint64_t height() const { return first.height; }

    bool operator==(const Equivocation &other) const { return first == other.first && second == other.second; }

    template <typename Archive> void serialize(Archive &ar) { ar & first & second; }

    /** Versioned bytes (disk, STATUS `equivocation`, EQUIVOCATION_ADD). */
    std::string ltsToString() const;
    bool ltsFromString(const std::string &str);
  };

  /**
   * BLOCK_WAIT and BLOCK_ADD reply: the replying node's next block id and the latest signed tip
   * it holds (relays pass the beacon's on unchanged).
   */
  struct TipReply {
    uint64_t nextBlockId{0};
    bool hasSignedTip{false};
    SignedTip signedTip;

    std::optional<SignedTip> tip() const { return hasSignedTip ? std::optional<SignedTip>(signedTip) : std::nullopt; }
    void setTip(const std::optional<SignedTip> &tip) {
      hasSignedTip = tip.has_value();
      signedTip = tip.value_or(SignedTip{});
    }

    template <typename Archive> void serialize(Archive &ar) {
      ar & nextBlockId & hasSignedTip & signedTip;
    }
  };

  /**
   * A record that changed the beacon authority (T_CONFIG, T_BEACON_CERT, T_RECOVERY), its block and
   * that block's epoch (a recovery applies by epoch, ba7).
   */
  struct AuthorityStep {
    uint64_t blockId{0};
    uint64_t epoch{0};
    Ledger::Record record;

    template <typename Archive> void serialize(Archive &ar) { ar & blockId & epoch & record; }
  };

  /**
   * AUTHORITY_PROOF reply (beacon-authority ba5b): the authority steps from genesis up to a block,
   * in chain order. Each step is admin-signed, so a fresh node replays them from a pinned genesis
   * to check a checkpoint's authority without the blocks in between.
   */
  struct AuthorityProof {
    std::vector<AuthorityStep> steps;

    template <typename Archive> void serialize(Archive &ar) { ar & steps; }
  };

  /**
   * A recovery pending on chain (beacon-authority ba7), as STATUS shows it: the recovery set
   * announced a new admin set at `announcedAt`; it replaces the admin set at the first block of
   * `applyEpoch`, and until then the admin set is frozen.
   */
  struct RecoveryStatus {
    uint64_t announcedAt{0};
    uint64_t applyEpoch{0};
    std::vector<std::string> adminPublicKeys;
    uint8_t adminMinSignatures{0};

    bool operator==(const RecoveryStatus &other) const {
      return announcedAt == other.announcedAt && applyEpoch == other.applyEpoch &&
             adminPublicKeys == other.adminPublicKeys && adminMinSignatures == other.adminMinSignatures;
    }
    template <typename Archive> void serialize(Archive &ar) {
      ar & announcedAt & applyEpoch & adminPublicKeys & adminMinSignatures;
    }
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
    /** The beacon's latest signed tip (a relay serves the one it holds); absent before the first. */
    std::optional<SignedTip> signedTip;
    /**
     * The current beacon certificate (beacon-authority ba5c): the T_BEACON_CERT record of the
     * highest generation this node has seen out of band, else the latest one on chain it holds;
     * absent at generation 1. Self-verifying: the receiver checks the admin signatures.
     */
    std::optional<Ledger::Record> certificate;
    /**
     * Equivocation evidence this node halts on (beacon-authority ba6); absent if none. Receivers
     * check it against their own authority and halt with it.
     */
    std::optional<Equivocation> equivocation;
    /** A recovery pending on this node's chain (beacon-authority ba7); absent if none. */
    std::optional<RecoveryStatus> recovery;

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
  /** AUTHORITY_PROOF up to `upToBlockId`. */
  Roe<AuthorityProof> fetchAuthorityProof(uint64_t upToBlockId);
  Roe<UserAccount> fetchUserAccount(const uint64_t accountId);
  Roe<TxGetByWalletResponse> fetchTransactionsByWallet(const TxGetByWalletRequest &request);
  Roe<Ledger::Record> fetchTransactionByIndex(const TxGetByIndexRequest &request);

  Roe<void> addTransaction(const Ledger::Record &record);
  Roe<TipReply> addBlock(const Ledger::ChainNode& block);

  /**
   * Async variants: start the call and return; `done` runs once with the
   * result, possibly on a transport thread (AMP io lane) — hop to your own
   * thread before touching state.
   */
  template <typename T> using Done = std::function<void(Roe<T>)>;
  void registerMinerServerAsync(const MinerInfo &minerInfo, Done<BeaconState> done);
  void fetchMinerListAsync(Done<std::vector<MinerInfo>> done);
  void addTransactionAsync(const Ledger::Record &record, Done<void> done);
  /** EQUIVOCATION_ADD: push equivocation evidence up (beacon-authority ba6). Returns the reply text. */
  Roe<std::string> addEquivocation(const Equivocation &evidence);
  void addEquivocationAsync(const Equivocation &evidence, Done<std::string> done);
  /** BLOCK_ADD: the reply carries the beacon's signed tip once it has committed the block. */
  void addBlockAsync(const Ledger::ChainNode &block, Done<TipReply> done);
  /** TX_PULL: pending transactions for the leader of `slot`. */
  void pullTransactionsAsync(uint64_t slot, Done<std::vector<Ledger::Record>> done);
  void fetchCalibrationAsync(Done<CalibrationResponse> done);
  void fetchBlockAsync(uint64_t blockId, Done<Ledger::ChainNode> done);
  void fetchBeaconStateAsync(Done<BeaconState> done);
  /** BLOCK_WAIT: `done` gets the upstream's next block id once it passes `knownNextBlockId` (or unchanged after a hold). */
  void waitForBlockAsync(uint64_t knownNextBlockId, Done<TipReply> done);

  /** True when the call failed before a server answered (worth retrying later). */
  static bool isTransportError(int32_t code) {
    return code == E_NOT_CONNECTED || code == E_REQUEST_FAILED;
  }

private:
  static Roe<BeaconState> parseBeaconState(const std::string &payload);
  static Roe<CalibrationResponse> parseCalibration(const std::string &payload);
  static Roe<Ledger::ChainNode> parseBlock(const std::string &payload);
  static Roe<TipReply> parseTipReply(const std::string &payload);
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
