#ifndef PP_LEDGER_GATEWAY_H
#define PP_LEDGER_GATEWAY_H

// The verifying gateway (projects/finality FN004–FN006): what a service beside the chain may rely on,
// each answer checked as `pp-client verified-account` checks it — block 0 against a pinned hash, the
// beacon authority replayed from it, signed tips, the blocks under them hash by hash, accounts by their
// proofs. pp-http serves it on /v1/.

#include "ledger/chain/AuthorityTrail.h"
#include "ledger/chain/RecordHandler.h"
#include "ledger/chain/Types.h"
#include "ledger/client/Client.h"
#include "ledger/ledger/Ledger.h"
#include "common/Logger.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pp::gateway {

/** Where the gateway reads the chain: a node through pp::Client, or a chain in tests. Nothing it says
 *  is believed before it is checked. */
class Source {
public:
  template <typename T> using Roe = ResultOrError<T, Client::Error>;

  virtual ~Source() = default;
  virtual Roe<Client::BeaconState> status() = 0;
  virtual Roe<Ledger::ChainNode> block(uint64_t height) = 0;
  virtual Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) = 0;
  /** At the node's tip: the proof names its block. */
  virtual Roe<Client::AccountProof> accountProof(uint64_t accountId) = 0;
};

/** A node, through a pp::Client of the gateway's own (its calls block; one thread uses it). */
class ClientSource : public Source {
public:
  explicit ClientSource(Client &client) : client_(client) {}
  Roe<Client::BeaconState> status() override { return client_.fetchBeaconState(); }
  Roe<Ledger::ChainNode> block(uint64_t height) override { return client_.fetchBlock(height); }
  Roe<Client::AuthorityProof> authorityProof(uint64_t upToBlockId) override {
    return client_.fetchAuthorityProof(upToBlockId);
  }
  Roe<Client::AccountProof> accountProof(uint64_t accountId) override { return client_.fetchAccountProof(accountId); }

private:
  Client &client_;
};

/** Something a service must know about (GET /v1/events). */
struct Event {
  enum Kind : uint8_t {
    /** A later generation's certificate, held: `generation`, `height` its cutover (none: kNoCutover). */
    Certificate = 1,
    /** Blocks above `height` dropped up to `droppedUpTo` for `generation`; `crossedFinality` if a final
     *  one was among them. */
    Rewind = 2,
    /** The beacon of `generation` signed two blocks at `height`: nothing above it is followed until a
     *  later generation is certified. */
    Equivocation = 3,
    /** A certificate vouches for another block at `height` than the one verified here: the gateway
     *  stops; its operator decides (a new data directory follows the chain again). */
    Forked = 4,
    /** `accountId`'s balances changed by `height` without a record the gateway saw naming it: heights
     *  from `droppedUpTo` (the first not known) to `height` are not known. */
    Unexplained = 5,
  };

  uint64_t seq{0};
  uint8_t kind{0};
  uint64_t generation{0};
  uint64_t height{0};
  uint64_t droppedUpTo{0};
  bool crossedFinality{false};
  uint64_t accountId{0};

  template <typename Archive> void serialize(Archive &ar) {
    ar & seq & kind & generation & height & droppedUpTo & crossedFinality & accountId;
  }
  static const char *kindName(uint8_t kind);
};

/** A verified block's facts (GET /v1/blocks/{height}). */
struct BlockView {
  uint64_t height{0};
  std::string hash; ///< raw
  uint64_t slot{0};
  int64_t slotTime{0};
  /** Its header's finalAt (FN007): from then on it and every block below it are final. */
  int64_t finalityTime{0};
  bool final{false};
};

/** The latest verified tip (GET /v1/tip). */
struct TipView {
  BlockView block;
  uint64_t generation{0};
  /** The highest verified block whose header's finalAt has passed by the gateway's clock (FN007).
   *  None while none has. */
  std::optional<uint64_t> finalizedHeight;
  /** The bound in force at the tip's epoch, as the config updates followed say: informational —
   *  finality comes from the headers. */
  uint64_t finalitySeconds{0};
  /** Why the gateway follows no further (an equivocation, a fork); empty while it follows. */
  std::string halted;
};

/** An account's balances at a height (GET /v1/accounts/{id}/balances). */
struct BalancesView {
  uint64_t accountId{0};
  uint64_t height{0};
  /** False: no such account at that height (no balances). */
  bool present{false};
  std::map<uint64_t, int64_t> balances;
};

/** Which height a balance is asked at. */
struct At {
  enum Kind { Final, Tip, Height, Time };
  Kind kind{Tip};
  int64_t value{0}; ///< Height: the height; Time: unix seconds
};

class Gateway {
public:
  /** Refusals, as the HTTP statuses /v1/ answers with. */
  enum Code : int32_t { kBadRequest = 400, kNotFound = 404, kConflict = 409, kUnavailable = 503 };
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  struct Options {
    /** Block 0's hash, raw (32 bytes), from a source the service trusts. */
    std::string genesisHash;
    /** Where the journal lives (created if missing). */
    std::filesystem::path dataDir;
    /** Every watched account is proven again this often: a write the gateway did not see coming shows
     *  as an `Unexplained` event. */
    int64_t sweepSeconds{300};
    /** Final blocks older than this are forgotten (with balances only they held): /v1/ answers
     *  from the oldest block kept on. */
    int64_t keepFinalSeconds{30LL * 24 * 3600};
    /** The journal is rewritten from what is held once it grows past this, and past twice its size
     *  after the last rewrite. */
    uint64_t compactAfterBytes{16ULL * 1024 * 1024};
  };

  Gateway(Source &source, Options options);
  ~Gateway();
  Gateway(const Gateway &) = delete;
  Gateway &operator=(const Gateway &) = delete;

  /** Load the journal (if any) and check block 0 against the pinned hash. */
  Roe<void> open();

  /**
   * One round: the node's STATUS (its certificate and equivocation evidence first, then its signed
   * tip), the blocks up to that tip, the watched accounts they name. Call from one thread; the views
   * below may be read from others meanwhile.
   */
  Roe<void> poll();

  /** Fixed "now" for tests (none: the system clock). */
  void setClockOverride(std::optional<int64_t> fixed);

  Roe<TipView> tip() const;
  Roe<BlockView> blockAt(uint64_t height) const;
  Roe<BalancesView> balances(uint64_t accountId, At at) const;
  /** Up to `limit` events after `afterSeq`. */
  std::vector<Event> events(uint64_t afterSeq, size_t limit) const;

  /** Watch from the current tip on: the next round proves it. Idempotent. */
  Roe<void> watch(uint64_t accountId);
  /** Forget it and what was recorded for it. */
  Roe<void> unwatch(uint64_t accountId);
  std::vector<uint64_t> watched() const;

private:
  struct Facts {
    uint64_t height{0};
    std::string hash;
    uint64_t slot{0};
    uint64_t epoch{0};
    std::string stateRoot;
    int64_t finalAt{0};
  };
  /** Balances from a height on, until the next entry. */
  struct Entry {
    /** False: the account changed more than once between two proofs; its balances here are not known. */
    bool known{true};
    /** False: no such account. */
    bool present{false};
    std::map<uint64_t, int64_t> balances;
    bool operator==(const Entry &other) const = default;
  };
  struct Account {
    /** Absent: not proven yet. */
    std::optional<uint64_t> provenUpTo;
    std::map<uint64_t, Entry> entries;
    /** Blocks above `provenUpTo` whose records name it. */
    std::set<uint64_t> touches;
  };
  /** A journal entry (one append per change; replayed on open). */
  struct Record;

  Roe<void> bootstrap();
  /** The authority at the tip held: the steps replayed from block 0, the certificate and evidence held. */
  Roe<void> rebuildTrail();
  /**
   * Whether the certificate held out of band still stands on `trail` (as a node re-checks it,
   * Chain::refreshSeenCertificate): a later generation than the one in force, signed by the admin
   * set in force, no recovery pending. Why not, when it does not.
   */
  Roe<void> heldCertificateStands(const AuthorityTrail &trail) const;
  /** Journals that no certificate is held any more. */
  Roe<void> dropHeldCertificate(const std::string &why);
  Roe<void> offerCertificate(const Ledger::Record &record);
  Roe<void> offerEquivocation(const Client::Equivocation &evidence);
  void refreshEquivocation();
  Roe<void> rewindTo(const BeaconCertificate &cert);
  /** Blocks from the tip held to `tip`, checked bottom up; kept only if `tip` verifies over them. */
  Roe<void> advance(const Client::SignedTip &signedTip);
  /** Prove the watched accounts due: named by new blocks, new, or all at a sweep. True if a node's
   *  proof was of a block not verified here yet (the next round takes it). */
  Roe<bool> proveAccounts();
  void recordProof(uint64_t accountId, const Client::Wallet *wallet, uint64_t provenAt);
  void updateFinalized();
  void rebuildFinalityHistory();

  Roe<void> loadJournal();
  /** Rewrite the journal from what is held (it only grows otherwise). */
  Roe<void> compact();
  std::vector<Record> snapshot() const;
  void apply(const Record &record);
  Roe<void> append(const Record &record);
  void emit(Event event);

  int64_t now() const;
  int64_t slotTime(uint64_t slot) const;
  uint64_t finalitySecondsAt(uint64_t epoch) const;
  const Facts *factsAt(uint64_t height) const;
  BlockView view(const Facts &facts) const;
  Roe<uint64_t> resolve(At at) const;
  std::string haltedReason() const;

  Source &source_;
  Options options_;
  mutable std::mutex mutex_;
  /** Appended to (binary); closed and reopened when the journal is rewritten. */
  std::ofstream journal_;
  uint64_t journalBytes_{0};
  uint64_t compactedBytes_{0};
  std::optional<int64_t> clockOverride_;
  RecordHandler records_;

  // Block 0 and the config it set.
  std::optional<Ledger::ChainNode> genesis_;
  BlockChainConfig config_;

  // The chain followed (written by poll's thread only, under mutex_).
  std::vector<Facts> facts_; ///< from facts_.front().height (the gateway's start) up to its tip
  std::vector<Client::AuthorityStep> steps_;
  std::optional<AuthorityTrail> trail_;
  std::optional<Client::SignedTip> lastTip_;
  std::optional<BeaconCertificate> seen_;
  std::optional<Ledger::Record> seenRecord_;
  std::optional<Client::Equivocation> equivocation_;
  std::optional<uint64_t> forkedAt_;
  /** (fromEpoch, seconds), from genesis and the config updates followed: informational only. */
  std::vector<std::pair<uint64_t, uint64_t>> finalityHistory_;
  std::optional<uint64_t> finalized_;

  std::map<uint64_t, Account> accounts_;
  int64_t lastSweep_{0};
  std::vector<Event> events_;
  uint64_t nextSeq_{1};

  mutable logging::Logger log_;
};

} // namespace pp::gateway

#endif // PP_LEDGER_GATEWAY_H
