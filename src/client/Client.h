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
#include "../ledger/Account.h"
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
  /** Account state types (src/ledger/Account.h): no network code needed to read or build them. */
  using MinerState = pp::MinerState;
  using Wallet = pp::Wallet;
  using UserAccount = pp::UserAccount;

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
    /** When the node said when to try again (Response::retryAfterMs); 0: it did not. */
    uint32_t retryAfterMs{ 0 };
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Default connection settings
  static constexpr const char *DEFAULT_HOST = "localhost";
  static constexpr const uint16_t DEFAULT_BEACON_PORT = 8517;
  static constexpr const uint16_t DEFAULT_MINER_PORT = 8518;
  static constexpr const uint16_t DEFAULT_RELAY_PORT = 8519;


  // Request types
  static constexpr const uint32_t T_REQ_STATUS = 1;
  static constexpr const uint32_t T_REQ_REGISTER = 2;
  static constexpr const uint32_t T_REQ_MINER_LIST = 3;
  /** Request precise server timestamp in ms since epoch for time calibration. */
  static constexpr const uint32_t T_REQ_CALIBRATION = 4;
  /**
   * A node's health and gauges (NodeHealth). Answered at once, to any peer, from what the server
   * thread last published: never queued, so probes work while the node syncs or is busy.
   */
  static constexpr const uint32_t T_REQ_HEALTH = 5;

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

  /** A request type's name for logs and metric labels ("block_get"); "type_<n>" for one unknown. */
  static std::string requestTypeName(uint32_t type);

  // Overload and deadline codes (load-resilience LR006, LR007): sent by nodes in Response.errorCode,
  // seen by clients as their Error code. All mean "the request was not done; the same bytes may be
  // sent again" — after retryAfterMs when given.
  /** The node's queue is full (or it is stopping). */
  static constexpr const uint16_t E_BUSY = 10;
  /** This peer has more requests at the node than it takes from one peer. */
  static constexpr const uint16_t E_RATE_LIMITED = 11;
  /** The beacon's transaction pool (or this payer's share of it) is full. */
  static constexpr const uint16_t E_POOL_FULL = 12;
  /** A relay's upstream did not answer in time: it may have taken the request (resend the same bytes). */
  static constexpr const uint16_t E_UPSTREAM_TIMEOUT = 13;
  /** The request waited past its deadline at the node and was not served. */
  static constexpr const uint16_t E_EXPIRED = 14;
  /** Whether `code` is one of the codes above. */
  static bool isOverloadCode(int32_t code) { return code >= E_BUSY && code <= E_EXPIRED; }

  // Error codes
  static constexpr const uint16_t E_NOT_CONNECTED = 1;
  static constexpr const uint16_t E_INVALID_RESPONSE = 2;
  static constexpr const uint16_t E_SERVER_ERROR = 3;
  static constexpr const uint16_t E_PARSE_ERROR = 4;
  static constexpr const uint16_t E_REQUEST_FAILED = 5;

  // Get human-friendly error message for an error code
  static std::string getErrorMessage(uint16_t errorCode);

  /**
   * Version 2 (load-resilience LR006) adds `deadlineMs`: how long the client waits for the reply.
   * A version 1 request is still read (no deadline).
   */
  struct Request {
    static constexpr const uint32_t VERSION = 2;

    uint32_t version{ VERSION };
    uint32_t type{ 0 };
    std::string payload;
    /** Milliseconds the client waits from sending (0: unknown). A node forwards with what remains. */
    uint32_t deadlineMs{ 0 };
    /** Set by the receiving node, not on the wire: the sender's authenticated peer id ("" unknown). */
    std::string from;

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & version & type & payload;
      if (version >= 2) {
        ar & deadlineMs;
      }
    }
  };

  /**
   * The `type` of a packed Request without unpacking its payload (it follows
   * `version`, both big-endian u32). nullopt when the body is too short.
   */
  /**
   * A node's own chain work rather than a client's request (load-resilience LR009): blocks up the
   * tree, pulls for a slot leader, and the sync and state a follower keeps itself current with. On
   * the way to a peer it goes before forwarded client requests and has places they cannot take.
   */
  static bool isChainDuty(uint32_t type) {
    switch (type) {
    case T_REQ_BLOCK_ADD:
    case T_REQ_TX_PULL:
    case T_REQ_BLOCK_GET:
    case T_REQ_BLOCK_WAIT:
    case T_REQ_STATUS:
    case T_REQ_MINER_LIST:
    case T_REQ_AUTHORITY_PROOF:
    case T_REQ_EQUIVOCATION_ADD:
    case T_REQ_CALIBRATION:
      return true;
    default:
      return false;
    }
  }

  /**
   * A packed Request with its deadline cut to `remainingMs` (load-resilience LR006): a request that
   * waited before it was sent (its client's in-flight limit) tells the node what is left, not what
   * it started with — else a relay forwards with time its client no longer has. Bytes that are not
   * exactly a packed Request, a version 1 one, or one with no deadline are returned unchanged.
   */
  static std::string withRemainingDeadline(const std::string &body, uint32_t remainingMs);

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

  /**
   * Version 2 (load-resilience LR007) adds `retryAfterMs`. A version 1 response is still read.
   * `errorCode` 0: `payload` is the reply; else `payload` is the message. Codes on the wire: 1, 3
   * (any error), and those below; a client sees the ones below as its Error code.
   */
  struct Response {
    static constexpr const uint32_t VERSION = 2;
    uint32_t version{ VERSION };
    uint16_t errorCode{ 0 };
    std::string payload;
    /** With an overload code: when to try again (milliseconds; 0: none said). */
    uint32_t retryAfterMs{ 0 };

    template <typename Archive>
    void serialize(Archive &ar) {
      ar & version & errorCode & payload;
      if (version >= 2) {
        ar & retryAfterMs;
      }
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
    /** issuedAt may differ from the receiving node's clock by at most this much. */
    static constexpr std::chrono::minutes kMaxIssuedSkew{ 5 };
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
    /** The chain's network id (what transactions are signed for); the field BeaconState has too. */
    std::string networkId;

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
   * A rewind to a certificate's cutover (beacon-authority bp1, B022): the blocks after
   * `keptUpTo` up to `droppedUpTo` were signed by a superseded generation past its cutover and are
   * gone. The order a node writes before it restarts (`data/rewind.dat`), the record it keeps once
   * applied (`data/rewound.dat`), and what STATUS shows.
   */
  struct RewindReport {
    constexpr static const uint32_t VERSION = 1;

    uint64_t keptUpTo{0};    ///< the cutover: the last block kept
    uint64_t droppedUpTo{0}; ///< the node's last block when the rewind was ordered
    uint64_t generation{0};  ///< the certificate that ordered it

    bool operator==(const RewindReport &other) const {
      return keptUpTo == other.keptUpTo && droppedUpTo == other.droppedUpTo && generation == other.generation;
    }
    template <typename Archive> void serialize(Archive &ar) { ar & keptUpTo & droppedUpTo & generation; }

    std::string ltsToString() const;
    bool ltsFromString(const std::string &str);
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
    /** The last rewind this node applied (beacon-authority bp1); absent if none. */
    std::optional<RewindReport> rewound;

    pp::common::Meta ltsToMeta() const;
    Roe<bool> ltsFromMeta(const pp::common::Meta &meta);
  };

  /**
   * HEALTH: liveness, readiness and what pp-http exports as metrics
   * (docs/ops/CONFIGURATION.md § Health and metrics). The server thread publishes it about once a
   * second; reportAgeMs says how long ago, so a stuck server thread shows as not live.
   */
  struct NodeHealth {
    /** Not published for this long: the server thread is stuck. */
    static constexpr int64_t kStaleAfterMs = 30000;

    std::string role;      ///< beacon | relay | miner
    std::string peerId;    ///< the node's network identity
    std::string networkId; ///< the chain's
    bool ready{ false };
    std::string notReady;  ///< why not, when !ready
    uint64_t nextBlockId{ 0 };
    std::optional<uint64_t> upstreamNextBlockId; ///< relay: from the upstream's signed tip
    std::optional<int64_t> lastSyncAgeSeconds;   ///< relay, miner: since the last successful sync
    std::optional<uint64_t> signedTipHeight;
    uint64_t generation{ 0 };      ///< beacon generation in force
    bool halted{ false };          ///< equivocation evidence held (beacon-authority ba6)
    bool recoveryPending{ false }; ///< a recovery waits (ba7)
    bool standingDown{ false };    ///< beacon: another key is certified
    uint64_t requestQueueDepth{ 0 };
    int64_t reportAgeMs{ -1 };     ///< since the server thread published; -1: never

    // Load (load-resilience lr1). Counters run from the node's start.
    /** Requests of one type served (handled, whatever the answer). */
    struct RequestCount {
      uint32_t type{ 0 };
      uint64_t served{ 0 };
      uint64_t waitMs{ 0 };    ///< summed: time in the queue before the handler ran
      uint64_t serviceMs{ 0 }; ///< summed: time in the handler (to its reply, or to handing it on)
    };
    std::vector<RequestCount> requests;
    uint64_t requestsBusy{ 0 };    ///< answered "busy" before queueing: queue full, or stopping
    uint64_t requestsRefused{ 0 }; ///< refused unserved: too large, peer not allowed, malformed, unsupported, wrong direction
    uint64_t requestsExpired{ 0 }; ///< waited longer than its client would, and answered unserved
    uint64_t loopTurnsSlow{ 0 };   ///< run-loop turns longer than kSlowTurnMs (a slow handler or duty)
    uint64_t loopTurnMaxMs{ 0 };   ///< the longest run-loop turn in the last 10 to 20 s
    std::optional<uint64_t> rssBytes; ///< resident memory (where the platform tells)
    std::optional<uint64_t> udpReceiveBufferBytes; ///< the socket's receive buffer, as the OS granted it
    std::optional<uint64_t> udpReceiveDrops;       ///< datagrams the OS dropped for want of room (Linux)
    uint64_t minerPeers{ 0 }; ///< peers that registered a miner through this node lately: trusted (LR009)
    std::optional<uint64_t> links;                 ///< peer links held (at most maxLinks)
    // Miner
    std::optional<uint64_t> blocksSealed;    ///< blocks it produced and broadcast
    std::optional<uint64_t> blocksCommitted; ///< of those, committed on the beacon's signed tip
    std::optional<uint64_t> blocksAbandoned; ///< of those, taken by no upstream
    std::optional<int64_t> lastSealDelayMs;  ///< the last seal, after its slot ended
    // Beacon: its transaction pool
    std::optional<uint64_t> txPoolRecords;
    std::optional<uint64_t> txPoolBytes;
    std::optional<int64_t> txPoolOldestAgeSeconds;
    std::optional<uint64_t> txPoolAdded;      ///< counters, from the start
    std::optional<uint64_t> txPoolDuplicates;
    std::optional<uint64_t> txPoolFull;       ///< refused: the pool was full
    std::optional<uint64_t> txPoolExpired;
    std::optional<uint64_t> txPoolIncluded;   ///< left the pool in a committed block
    std::optional<uint64_t> txPoolSenderFull; ///< refused: its payer had its share pooled
    std::optional<uint64_t> txPoolDropped;    ///< removed: could no longer apply after a block

    /** A run-loop turn longer than this counts in loopTurnsSlow. */
    static constexpr int64_t kSlowTurnMs = 1000;

    /** The server thread is reporting. */
    bool live() const { return reportAgeMs >= 0 && reportAgeMs < kStaleAfterMs; }
    /** Ready to serve, as reported by a live server thread. */
    bool readyNow() const { return live() && ready; }

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
  /**
   * The network id the node's chain has (from STATUS, any role): what a transaction must be signed
   * for. An error when the node does not say — signed for an empty id, every node would refuse it.
   */
  Roe<std::string> fetchNetworkId();
  /** HEALTH of the node this client talks to (any role). */
  Roe<NodeHealth> fetchHealth();
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
  // `timeout` 0: the default for the request; else the budget a forwarding node has left (LR006).
  void registerMinerServerAsync(const MinerInfo &minerInfo, Done<BeaconState> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  void fetchMinerListAsync(Done<std::vector<MinerInfo>> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  void addTransactionAsync(const Ledger::Record &record, Done<void> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  /** EQUIVOCATION_ADD: push equivocation evidence up (beacon-authority ba6). Returns the reply text. */
  Roe<std::string> addEquivocation(const Equivocation &evidence);
  void addEquivocationAsync(const Equivocation &evidence, Done<std::string> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  /** BLOCK_ADD: the reply carries the beacon's signed tip once it has committed the block. */
  void addBlockAsync(const Ledger::ChainNode &block, Done<TipReply> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  /** TX_PULL: pending transactions for the leader of `slot`. */
  void pullTransactionsAsync(uint64_t slot, Done<std::vector<Ledger::Record>> done, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
  void fetchCalibrationAsync(Done<CalibrationResponse> done);
  void fetchBlockAsync(uint64_t blockId, Done<Ledger::ChainNode> done);
  void fetchBeaconStateAsync(Done<BeaconState> done);
  /** BLOCK_WAIT: `done` gets the upstream's next block id once it passes `knownNextBlockId` (or unchanged after a hold). */
  void waitForBlockAsync(uint64_t knownNextBlockId, Done<TipReply> done);

  /**
   * Any request, async: `done` gets the reply payload, or the server's error message (E_SERVER_ERROR)
   * or the transport's (E_REQUEST_FAILED). For tools that look at every reply (pp-client load).
   */
  void requestAsync(uint32_t type, const std::string &payload, Done<std::string> done,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  /**
   * TX_ADD with the one retry policy (load-resilience LR007): the same signed bytes are sent again
   * after an overload code, a deadline or a transport failure — honouring retryAfterMs, else
   * backing off (0.5 s doubling to 10 s), with jitter — until the node takes it, refuses it for good,
   * or `maxWait` (and the transaction's validity window) ends. The reply text, or the last error.
   * Never re-sign to retry: a new signature is a new transaction.
   */
  Roe<std::string> submitTransaction(const Ledger::Record &record,
                                     std::chrono::milliseconds maxWait = std::chrono::minutes(2));
  /** Whether `error` says the same request may be sent again. */
  static bool isRetryable(const Error &error) {
    return isOverloadCode(error.code) || isTransportError(error.code);
  }

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
  std::string packRequest(uint32_t type, const std::string &payload, std::chrono::milliseconds timeout);
  static Roe<std::string> parseResponse(const ILedgerTransport::Roe<std::string> &result);
  void sendRequestAsync(uint32_t type, const std::string &payload,
                        std::chrono::milliseconds timeout, Done<std::string> done);
  /** `timeout` when set, else `fallback`. */
  static std::chrono::milliseconds orDefault(std::chrono::milliseconds timeout, std::chrono::milliseconds fallback) {
    return timeout.count() > 0 ? timeout : fallback;
  }
  Roe<std::string> sendRequest(uint32_t type, const std::string &payload, std::chrono::milliseconds timeout);
  std::chrono::milliseconds fastTimeout() const { return requestTimeout_; }
  std::chrono::milliseconds dataTimeout() const { return network::NetworkTuning::dataTimeoutFor(requestTimeout_); }

  std::string amp_default_peer_key_{"remote"};
  std::unique_ptr<ILedgerTransport> transport_;
  std::chrono::milliseconds requestTimeout_{network::NetworkTuning::kDefaultRpcTimeout};
};

std::ostream& operator<<(std::ostream& os, const Client::Request& req);

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
