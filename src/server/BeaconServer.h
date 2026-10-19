#ifndef PP_LEDGER_BEACON_SERVER_H
#define PP_LEDGER_BEACON_SERVER_H

#include "server/Beacon.h"
#include "server/MinerRegistry.h"
#include "server/TxPool.h"
#include "server/Server.h"
#include "ledger/client/Client.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "common/ResultOrError.hpp"
#include "ledger/lib/common/Meta.h"
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
  /** Recovery delay when init-config omits `recovery.delayEpochs`: about 30 days (beacon-authority B007). */
  constexpr static const uint64_t DEFAULT_RECOVERY_DELAY_SECONDS = chain_genesis::defaults::kRecoveryDelaySeconds;
  /** Beacon signing key pair under the work dir (same format as `pp-client keygen -o`). */
  constexpr static const char *FILE_BEACON_SIGNING_KEY = "keys/beacon-signing.key";
  constexpr static const char *FILE_BEACON_SIGNING_PUB = "keys/beacon-signing.pub";
  /** Optional: a key staged for a rotation; signs once a certificate names it. */
  constexpr static const char *FILE_BEACON_SIGNING_NEXT_KEY = "keys/beacon-signing.next.key";
  constexpr static const char *FILE_BEACON_SIGNING_NEXT_PUB = "keys/beacon-signing.next.pub";

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
  Roe<void> init(const std::string& workDir, const std::vector<std::string>& genesisKeyFiles, bool force = false,
                 const std::string& importStatePath = "");

  /**
   * --plan-genesis: block 0 as init-config.json and this beacon's signing key say, starting at
   * `startTime` (Unix seconds), written unsigned to `outputPath` as a genesis operation file
   * (docs/contracts/OPERATION_FILE.md) for the admin keys to sign offline.
   */
  Roe<void> planGenesis(const std::string& workDir, const std::string& outputPath, int64_t startTime,
                        const std::string& notes = "");
  /**
   * --init --genesis-op: write block 0 from a genesis operation signed by the admin quorum. Its
   * generation-1 beacon key must be this beacon's; no genesis private key comes near this host.
   */
  Roe<void> initSigned(const std::string& workDir, const std::string& operationPath, bool force = false);

  Service::Roe<void> run(const std::string &workDir) override {
    return Server::run(workDir);
  }

  /**
   * pp-beacon --promote (beacon-authority bp3): this start turns a relay's work directory into the
   * beacon taking over. It checks that the directory can (checkPromotion) and this host's clock,
   * then keeps the relay's config.json as config.relay.json and writes the beacon's.
   */
  void setPromote(bool promote) { promote_ = promote; }
  /**
   * pp-beacon --accept-ledger-tip (beacon-authority bp4): with no data/signed-tip.dat, sign the
   * ledger's last block as the tip (Beacon::openTipSigning refuses that by default).
   */
  void setAcceptLedgerTip(bool accept) { acceptLedgerTip_ = accept; }

protected:
  bool useSignatureFile() const override { return false; }
  std::string getSignatureFileName() const override { return FILE_SIGNATURE; }
  std::string getLogFileName() const override { return FILE_LOG; }
  std::string getServerName() const override { return "BeaconServer"; }
  void fillHealth(Client::NodeHealth &health) const override;
  /** Writes only a beacon in charge takes (BLOCK_ADD, TX_PULL, REGISTER): refused once it stood down or halted. */
  Roe<void> admit(const Client::Request &request) const override;

  void runLoop() override;

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

  // Default configuration values: chain_genesis::defaults (the Genesis app starts from the same).
  constexpr static const uint64_t DEFAULT_SLOT_DURATION = chain_genesis::defaults::kSlotDuration;
  constexpr static const uint64_t DEFAULT_SLOTS_PER_EPOCH = chain_genesis::defaults::kSlotsPerEpoch;
  constexpr static const uint64_t DEFAULT_MAX_CUSTOM_META_SIZE = chain_genesis::defaults::kMaxCustomMetaSize;
  constexpr static const uint64_t DEFAULT_MAX_TRANSACTIONS_PER_BLOCK = chain_genesis::defaults::kMaxTransactionsPerBlock;
  constexpr static const uint64_t DEFAULT_MAX_BLOCK_BYTES = chain_genesis::defaults::kMaxBlockBytes;
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_A = chain_genesis::defaults::kMinFeeCoefficients[0];
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_B = chain_genesis::defaults::kMinFeeCoefficients[1];
  constexpr static const uint16_t DEFAULT_MIN_FEE_COEFF_C = chain_genesis::defaults::kMinFeeCoefficients[2];
  constexpr static const uint64_t DEFAULT_FREE_CUSTOM_META_SIZE = chain_genesis::defaults::kFreeCustomMetaSize;

  // Checkpoint configuration values
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_BLOCKS = chain_genesis::defaults::kCheckpointMinBlocks;
  constexpr static const uint64_t DEFAULT_CHECKPOINT_MIN_AGE_SECONDS = chain_genesis::defaults::kCheckpointMinAgeSeconds;
  constexpr static const uint64_t DEFAULT_MAX_VALIDATION_TIMESPAN_SECONDS = chain_genesis::defaults::kMaxValidationTimespanSeconds;
  /** Default miner due period when omitted: one day of slots. */
  constexpr static const uint64_t DEFAULT_MINER_DUE_SECONDS = chain_genesis::defaults::kMinerDueSeconds;
  constexpr static const uint64_t DEFAULT_MINER_MAX_MISSED_SLOTS = chain_genesis::defaults::kMinerMaxMissedSlots;

  struct InitFileConfig {
    /** This chain's name; part of genesis (signatures and the epoch seed bind to it). */
    std::string networkId{ "local-devnet" };
    uint64_t slotDuration{ DEFAULT_SLOT_DURATION };
    uint64_t slotsPerEpoch{ DEFAULT_SLOTS_PER_EPOCH };
    uint64_t maxCustomMetaSize{ DEFAULT_MAX_CUSTOM_META_SIZE };
    uint64_t maxTransactionsPerBlock{ DEFAULT_MAX_TRANSACTIONS_PER_BLOCK };
    uint64_t maxBlockBytes{ DEFAULT_MAX_BLOCK_BYTES };
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
  constexpr static const uint64_t GENESIS_MINER_STAKE_SHARE = chain_genesis::defaults::kGenesisMinerStakeShare;

  /** config.json: the fields every node reads (NodeConfig). */
  struct RunFileConfig : NodeConfig {
    RunFileConfig() : NodeConfig(Client::DEFAULT_BEACON_PORT) {}
  };

  struct NetworkConfig {
    uint16_t udp_port{ Client::DEFAULT_BEACON_PORT };
  };

  struct Config {
    NetworkConfig network;
    Chain::CheckpointConfig checkpoint;
  };

  /** init-config.json (and a re-genesis's snapshot) as the genesis it describes, with no signer yet. */
  Roe<Beacon::InitConfig> prepareInit(const std::string& workDir, const std::string& importStatePath);

  /** `tipKeys`: sign the tip the init wrote (a re-genesis's last import block, B032); none: unsigned. */
  Roe<void> initFromWorkDir(const Beacon::InitConfig& config, bool force,
                            const std::vector<utl::MlDsaKeyPair>* tipKeys = nullptr);

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

  /** Serves BLOCK_WAIT: downstreams learn of each committed block at once. */
  std::optional<uint64_t> blockWaitTip() const override { return beacon_.getNextBlockId(); }
  std::optional<Client::SignedTip> latestSignedTip() const override { return beacon_.getSignedTip(); }
  /** BLOCK_ADD reply: next block id and the signed tip (which covers the block just added). */
  std::string tipReply() const;
  /** Why a write is refused while halted on equivocation evidence above `above` (ba6). */
  std::string haltedMessage(uint64_t above) const;

  Roe<std::string> hBlockAdd(const Client::Request &request);
  Roe<std::string> hStatus(const Client::Request &request);
  Roe<std::string> hRegister(const Client::Request &request);
  Roe<std::string> hMinerList(const Client::Request &request);
  /** TX_ADD reached the top of the tree: hold it in the pool for the slot leader (E_POOL_FULL when it cannot). */
  void dTxAdd(const Client::Request &request, const RequestQueue::Reply &reply);
  /** When a full pool asks a client to try again (LR007): the blocks it takes to drain, in slots. */
  uint32_t poolRetryAfterMs() const;
  /** EQUIVOCATION_ADD (ba6): keep the evidence if it verifies; the beacon halts on it. */
  Roe<std::string> hEquivocationAdd(const Client::Request &request);
  /** A T_BEACON_CERT in a TX_ADD (ba5c): kept if it is the highest seen; refused if it is not above. */
  Roe<void> offerCertificate(const Ledger::Record &record);
  /** TX_PULL: pending transactions for the slot leader (through its relay). */
  Roe<std::string> hTxPull(const Client::Request &request);
  /** After a commit: drop pooled records that can no longer apply (load-resilience lr4). */
  void dropStalePoolRecords(const std::vector<Ledger::Record> &committed);

  Config config_;
  Beacon beacon_;
  /** --promote's clock check (one dial key per node asked). */
  Client client_;
  bool promote_{false};
  bool acceptLedgerTip_{false};

  MinerRegistry miners_;
  TxPool txPool_;
  /** A miner record not renewed this long is dropped (miners renew every MinerServer::REGISTER_RENEW_INTERVAL). */
  static constexpr std::chrono::minutes MINER_RECORD_TTL{5};
};

} // namespace pp

#endif // PP_LEDGER_BEACON_SERVER_H
