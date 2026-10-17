#ifndef PP_LEDGER_LOAD_GENERATOR_H
#define PP_LEDGER_LOAD_GENERATOR_H

#include "Client.h"
#include "../ledger/Ledger.h"
#include "common/ResultOrError.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pp::load {

/**
 * pp-client load (projects/load-resilience, lr2): requests sent at a fixed rate whatever the replies
 * (open loop — a slow server shows as latency and errors, not as a slower client), from many client
 * identities, with every reply classified and transactions followed into blocks.
 */

/** What a request may be in a mix. */
enum class Kind {
  TxAdd,      ///< a valid transfer between two funded accounts
  TxJunk,     ///< a transfer that decodes but is badly signed: the pool takes it, no block will
  Status,
  AccountGet,
  BlockGet,
  TxByWallet, ///< wallet history: scans the chain backwards on the server thread
  Big,        ///< a BLOCK_ADD of ~4 MiB that is no block: the node receives it whole, then refuses it
  BlockWait,  ///< a BLOCK_WAIT at the tip: parked until the next block or the hold ends
  Register,   ///< a miner registration for a funded account, badly signed: forwarded to the beacon, which refuses it
};

std::string kindName(Kind kind);
std::optional<Kind> kindFromName(const std::string &name);

/** Weighted kinds: "tx_add=10,status=1". */
using Mix = std::vector<std::pair<Kind, uint32_t>>;
pp::Roe<Mix> parseMix(const std::string &spec);

/** What became of one request, from its reply. */
enum class Outcome {
  Taken,     ///< TX_ADD: pooled ("Transaction submitted")
  Duplicate, ///< TX_ADD: already pooled
  PoolFull,  ///< TX_ADD: the beacon's pool is full
  Served,    ///< a read answered
  Busy,      ///< a queue on the way was full
  Expired,   ///< waited past its client in a server queue
  Timeout,   ///< sent, and no reply in time
  Unsent,    ///< never sent: no link (dial timed out or backing off), no channel, no free place to the peer
  Refused,   ///< any other error
};

std::string outcomeName(Outcome outcome);
Outcome classify(Kind kind, const Client::Roe<std::string> &reply);

/** Latency samples, ms. */
class Latencies {
public:
  void add(double ms) { samples_.push_back(ms); }
  size_t count() const { return samples_.size(); }
  /** `q` in [0, 1]; 0 with no sample. */
  double quantile(double q) const;
  double max() const;

private:
  std::vector<double> samples_;
};

/** A funded account the generator signs for. */
struct Account {
  uint64_t id{0};
  std::string privateKey; ///< raw ML-DSA-65
};

/** accounts.json: {"networkId": ..., "accounts": [{"id": n, "key": "<hex>"}]}. */
pp::Roe<void> writeAccounts(const std::string &path, const std::string &networkId, const std::vector<Account> &accounts);
pp::Roe<std::vector<Account>> readAccounts(const std::string &path, std::string &networkId);

/** A transfer of `amount` from `from` to `to` with `fee`, signed for `networkId` (badly when `junk`). */
pp::Roe<Ledger::Record> transfer(const Account &from, uint64_t to, uint64_t amount, uint64_t fee,
                                 const std::string &networkId, bool junk = false);

struct RunConfig {
  std::string host;     ///< multiaddr of the node to load (a relay, or the beacon)
  Mix mix{{Kind::TxAdd, 1}};
  double rate{10};      ///< requests per second, all kinds
  std::chrono::seconds duration{10};
  uint64_t burst{0};    ///< transfers sent at once, at burstAt
  std::chrono::seconds burstAt{0};
  size_t peers{4};      ///< client identities (the server's limits are per peer)
  uint64_t amount{1};
  uint64_t fee{1};
  /**
   * After the last reply: follow blocks until every valid transfer sent is included, or none has
   * been for `quiet`, at most `drain`.
   */
  std::chrono::seconds drain{60};
  std::chrono::seconds quiet{15};
};

struct KindReport {
  uint64_t sent{0};
  std::map<Outcome, uint64_t> outcomes;
  Latencies latencyMs;
  /** Error replies by message (numbers masked, cut to 100 characters), to see what the outcomes stand for. */
  std::map<std::string, uint64_t> errors;
};

struct RunReport {
  std::map<Kind, KindReport> kinds;
  double seconds{0};
  uint64_t validSent{0}; ///< valid transfers sent
  uint64_t taken{0};     ///< of those, answered as pooled
  uint64_t included{0};  ///< of those sent, seen in a block (taken or not)
  uint64_t includedNotTaken{0}; ///< included although the reply was an error: a retry would pay twice
  Latencies inclusionMs;     ///< from send to the block, as the loaded node served it
  Latencies inclusionBlocks; ///< blocks from the tip at send to the block that included it
  uint64_t firstBlock{0};
  uint64_t lastBlock{0};
  uint64_t junkIncluded{0}; ///< must stay 0

  std::string text() const;
  pp::common::Object json() const;
};

/** Run the load on `cfg.host` with `accounts` (at least two for transfers). */
pp::Roe<RunReport> run(const RunConfig &cfg, const std::vector<Account> &accounts);

struct SetupConfig {
  std::string host;
  uint64_t from{0};                     ///< the funding account
  std::vector<std::string> fromKeys;    ///< its private keys (as many as it needs)
  size_t accounts{100};
  uint64_t amount{100000};
  uint64_t fee{1};
  std::chrono::seconds timeout{300};
};

/** Create `cfg.accounts` funded accounts (T_NEW_USER from `cfg.from`); returns those that exist. */
pp::Roe<std::vector<Account>> setup(const SetupConfig &cfg, std::string &networkId);

} // namespace pp::load

#endif // PP_LEDGER_LOAD_GENERATOR_H
