#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "Beacon.h"
#include "MinerRegistry.h"
#include "TxPool.h"
#include "Server.h"
#include "../client/Client.h"
#include "../network/amp/AmpIdentity.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Meta.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

namespace pp {

class BeaconServer : public Server {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  // Error codes
  static constexpr const int32_t E_CONFIG = -1;
  /** Recovery delay when init-config omits `recovery.delayEpochs`: about 30 days (beacon-authority B007). */
  constexpr static const uint64_t DEFAULT_RECOVERY_DELAY_SECONDS = 30ULL * 24 * 60 * 60;
  /** Beacon signing key pair under the work dir (same format as `pp-client keygen -o`). */
  constexpr static const char *FILE_BEACON_SIGNING_KEY = "keys/beacon-signing.key";
  constexpr static const char *FILE_BEACON_SIGNING_PUB = "keys/beacon-signing.pub";
  /** Optional: a key staged for a rotation; signs once a certificate names it. */
  constexpr static const char *FILE_BEACON_SIGNING_NEXT_KEY = "keys/beacon-signing.next.key";
  constexpr static const char *FILE_BEACON_SIGNING_NEXT_PUB = "keys/beacon-signing.next.pub";
  static constexpr const int32_t E_NETWORK = -2;
  static constexpr const int32_t E_BEACON = -3;
  static constexpr const int32_t E_REQUEST = -4;

  BeaconServer();
  ~BeaconServer() override = default;

  /**
   * Initialize (or reinitialize) the beacon from init-config.json: genesis
   * declares the system accounts and genesis miners by public key, and is
   * signed with the genesis private keys in `genesisKeyFiles`. The beacon
   * generates and stores no account keys.
   */
  /**
   * --init: write genesis into `workDir`/data. Refuses when data/ already holds a chain (a second
   * --init would destroy it) unless `force`.
   */
  Roe<void> init(const std::string& workDir, const std::vector<std::string>& genesisKeyFiles, bool force = false);

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

  /**
   * pp-beacon --promote (beacon-authority bp3): this start turns a relay's work directory into the
   * beacon taking over. It checks that the directory can (checkPromotion) and this host's clock,
   * then keeps the relay's config.json as config.relay.json and writes the beacon's.
   */
  void setPromote(bool promote) { promote_ = promote; }

protected:
  bool useSignatureFile() const override { return false; }
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "BeaconServer"; }
  int32_t getRunErrorCode() const override { return E_BEACON; }

  void runLoop() override;
  /**
   * When the held certificate orders a rewind (beacon-authority bp1), write the order and restart
   * after kRewindRestartGrace. Called once the role has loaded, when a sync finishes (startup syncs
   * too: a node back online with blocks past the cutover cannot sync), and from the run loop.
   */
  void orderRewindIfDue();

  /**
   * Called before service thread starts - initializes beacon and network
   */
  Service::Roe<void> onStart() override;

  /**
   * Called after service thread stops - cleans up resources
   */
  void onStop() override;

private:
  constexpr static const char* FILE_INIT_CONFIG = "init-config.json";
  constexpr static const char* FILE_CONFIG = "config.json";
  /** --promote keeps the relay's config.json here. */
  constexpr static const char* FILE_RELAY_CONFIG = "config.relay.json";
  constexpr static const char* FILE_LOG = "beacon.log";
  constexpr static const char* FILE_SIGNATURE = ".signature";
  constexpr static const char* DIR_DATA = "data";

  // Default configuration values
  constexpr static const uint64_t DEFAULT_SLOT_DURATION = 7; // 7 seconds per slot
  constexpr static const uint64_t DEFAULT_SLOTS_PER_EPOCH = static_cast<uint64_t>(24) * 3600; // 7 days per epoch
  constexpr static const uint64_t DEFAULT_MAX_CUSTOM_META_SIZE = static_cast<uint64_t>(1) * 1024 * 1024; // 1MB
  constexpr static const uint64_t DEFAULT_MAX_TRANSACTIONS_PER_BLOCK = static_cast<uint64_t>(10) * 1024; // 10K
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_A = 1;  // Base fee per transaction
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_B = 1;  // Fee per custom meta KiB
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_C = 1;  // Fee per custom meta KiB^2
  constexpr static const uint64_t DEFAULT_FREE_CUSTOM_META_SIZE = 1024; // 1KiB free custom meta size

  // Checkpoint configuration values
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_BLOCKS = 1 << 20; // 1 million blocks
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_AGE_SECONDS = static_cast<uint64_t>(365) * 24 * 3600; // 1 year (365 days)
  constexpr static const uint64_t DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS = 86400; // 24 hours
  /** Default miner due period when omitted: one day of slots. */
  constexpr static const uint64_t DEFAULT_MINER_DUE_SECONDS = 86400;
  constexpr static const uint64_t DEFAULT_MINER_MAX_MISSED_SLOTS = 3;

  struct InitFileConfig {
    /** This chain's name; part of genesis (signatures and the epoch seed bind to it). */
    std::string networkId{ "local-devnet" };
    uint64_t slotDuration{ DEFAULT_SLOT_DURATION };
    uint64_t slotsPerEpoch{ DEFAULT_SLOTS_PER_EPOCH };
    uint64_t maxCustomMetaSize{ DEFAULT_MAX_CUSTOM_META_SIZE };
    uint64_t maxTransactionsPerBlock{ DEFAULT_MAX_TRANSACTIONS_PER_BLOCK };
    std::vector<uint16_t> minFeeCoefficients{ DEFAULT_MIN_FEE_COEFF_A,
                                              DEFAULT_MIN_FEE_COEFF_B,
                                              DEFAULT_MIN_FEE_COEFF_C };
    uint64_t freeCustomMetaSize{ DEFAULT_FREE_CUSTOM_META_SIZE };
    uint64_t checkpointMinBlocks{ DEFAULT_CHECKPOINT_MIN_BLOCKS };
    uint64_t checkpointMinAgeSeconds{ DEFAULT_CHECKPOINT_MIN_AGE_SECONDS };
    uint64_t maxValidationTimespanSeconds{ DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS };
    /** Empty-seal lag; default (here and at parse time when omitted) = slotsPerEpoch. */
    uint64_t heartbeatSlots{ DEFAULT_SLOTS_PER_EPOCH };
    /** Genesis issuance cap per epoch (0 = none until a config update allows it). */
    uint64_t maxIssuancePerEpoch{0};
    /** Flat fee per account created after genesis (0 = none). */
    uint64_t newAccountFee{0};
    /** Miner registration and liveness (CONFIGURATION.md § Miners). */
    uint64_t minerMinStake{0};
    uint64_t minerRegistrationFee{0};
    /** Default (here and at parse time when omitted): one day of slots. */
    uint64_t minerDueAfterSlots{ (DEFAULT_MINER_DUE_SECONDS + DEFAULT_SLOT_DURATION - 1) / DEFAULT_SLOT_DURATION };
    uint64_t minerMaxMissedSlots{ DEFAULT_MINER_MAX_MISSED_SLOTS };
    uint64_t minerReregisterCooldownEpochs{0};
    /**
     * Miner accounts created in the genesis block (`genesisMiners`): the only
     * stake at start, since system accounts never lead. Stake defaults to an
     * equal share of GENESIS_MINER_STAKE_SHARE of the supply.
     */
    std::vector<Beacon::GenesisMiner> genesisMiners;
    /** Genesis, reserve, registrar, fee, recycle by public key (`systemAccounts`). */
    Beacon::InitKeyConfig systemAccounts;
    /** Recovery key set (`recovery`): keys, minSignatures (default all), delayEpochs. */
    Beacon::AccountKeys recovery;
    /** Default (at parse time when omitted): ~DEFAULT_RECOVERY_DELAY_SECONDS of epochs, at least 1. */
    uint64_t recoveryDelayEpochs{0};

    pp::common::Object ltsToJson();
    Roe<void> ltsFromJson(const pp::common::Object& jd);
    Roe<void> parseGenesisMiners(const pp::common::Object& jd);
    Roe<void> parseSystemAccounts(const pp::common::Object& jd);
    Roe<void> parseRecovery(const pp::common::Object& jd);
  };

  /** Default genesis miner stake: an equal share of 1/GENESIS_MINER_STAKE_SHARE of the supply. */
  constexpr static const uint64_t GENESIS_MINER_STAKE_SHARE = 10;

  // RunFileConfig can be autogenerated to help users understand the configuration file format.
  struct RunFileConfig {
    uint16_t port{ Client::DEFAULT_BEACON_PORT };

    /** `allowedPeers`: serve only these peer ids (and upstreams); absent = anyone. */
    std::optional<std::set<std::string>> allowedPeers;

    pp::common::Object ltsToJson();
    Roe<void> ltsFromJson(const pp::common::Object& jd);
  };

  struct NetworkConfig {
    uint16_t udp_port{ Client::DEFAULT_BEACON_PORT };
  };

  struct Config {
    NetworkConfig network;
    Chain::CheckpointConfig checkpoint;
  };

  Roe<void> initFromWorkDir(const Beacon::InitConfig& config, bool force);

  /** --promote: the relay's config.json — its port and allowedPeers kept, its upstreams to check the clock against. */
  struct Promotion {
    RunFileConfig config;
    std::vector<std::string> clockReferences;
  };
  Roe<Promotion> readRelayConfig() const;
  /**
   * --promote, before tip signing opens (it would sign with a key in force): no signed tip here; a
   * certificate held, with a cutover, names one of `keys` and the one in force none; block C held as
   * vouched.
   */
  Roe<void> checkPromotion(const std::vector<utl::MlDsaKeyPair> &keys) const;
  /**
   * --promote: this host's clock against what the relay's upstreams and the certificate's gateways
   * answer to CALIBRATION (their median); refused beyond the delivery window. None answering: warned.
   */
  Roe<void> checkClockForTakeover(std::vector<std::string> references);
  /** --promote: config.json becomes config.relay.json; the beacon's is written in its place. */
  Roe<void> writeBeaconConfig(RunFileConfig config) const;
  void initHandlers();

  /**
   * Record (or renew) a miner. Stamped with the beacon's clock; the registry
   * version changes only when the list does, so renewals cost nothing.
   */
  void registerServer(const Client::MinerInfo &minerInfo);
  /** Drop miners that have not renewed within MINER_RECORD_TTL. */
  void expireMinerRecords();
  Client::BeaconState buildStateResponse() const;

  std::string handleParsedRequest(const Client::Request &request) override;
  /** Serves BLOCK_WAIT: downstreams learn of each committed block at once. */
  std::optional<uint64_t> blockWaitTip() const override { return beacon_.getNextBlockId(); }
  std::optional<Client::SignedTip> latestSignedTip() const override { return beacon_.getSignedTip(); }
  /** BLOCK_ADD reply: next block id and the signed tip (which covers the block just added). */
  std::string tipReply() const;
  /** Why a write is refused while halted on equivocation evidence above `above` (ba6). */
  std::string haltedMessage(uint64_t above) const;

  Roe<std::string> hBlockGet(const Client::Request &request);
  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hAccountGet(const Client::Request &request);
  /** AUTHORITY_PROOF: authority-changing records from genesis up to a block (beacon-authority ba5b). */
  Roe<std::string> hAuthorityProof(const Client::Request &request);
  Roe<std::string> hTxGetByWallet(const Client::Request &request);
  Roe<std::string> hTxGetByIndex(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hCalibration(const Client::Request &request);
  Roe<std::string> hRegister(const Client::Request &request);
  Roe<std::string> hMinerList(const Client::Request &request);
  /** TX_ADD reached the top of the tree: hold it in the pool for the slot leader. */
  Roe<std::string> hTxAdd(const Client::Request &request);
  /** EQUIVOCATION_ADD (ba6): keep the evidence if it verifies; the beacon halts on it. */
  Roe<std::string> hEquivocationAdd(const Client::Request &request);
  /** A T_BEACON_CERT in a TX_ADD (ba5c): kept if it is the highest seen; refused if it is not above. */
  Roe<void> offerCertificate(const Ledger::Record &record);
  /** TX_PULL: pending transactions for the slot leader (through its relay). */
  Roe<std::string> hTxPull(const Client::Request &request);
  Roe<std::string> hUnsupported(const Client::Request &request);

  Config config_;
  Beacon beacon_;
  /** --promote's clock check (one dial key per node asked). */
  Client client_;
  bool promote_{false};

  using Handler = std::function<Roe<std::string>(const Client::Request &request)>;
  std::map<uint32_t, Handler> requestHandlers_;

  MinerRegistry miners_;
  TxPool txPool_;
  /** Most records one TX_PULL returns (a leader still caps what fits its block). */
  static constexpr size_t TX_PULL_MAX_RECORDS = 1000;
  /** A miner record not renewed this long is dropped (miners renew every MinerServer::REGISTER_RENEW_INTERVAL). */
  static constexpr std::chrono::minutes MINER_RECORD_TTL{5};
  /** A registration's issuedAt may differ from the beacon's clock by at most this much. */
  static constexpr std::chrono::minutes REGISTER_MAX_SKEW{5};
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
