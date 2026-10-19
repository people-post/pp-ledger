#include "ledger/client/LoadGenerator.h"
#include "ledger/client/AccountAttachment.h"
#include "ledger/client/AccountIds.h"
#include "ledger/network/LedgerAmpRuntime.h"
#include "ledger/network/amp/AmpIdentity.h"
#include "common/io/Json.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Crypto.h"
#include "ledger/lib/common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace pp::load {

using Clock = std::chrono::steady_clock;
using pp::common::Object;
using pp::common::Value;

// ============ names, mix, outcomes ============

namespace {

const std::vector<std::pair<Kind, std::string>> &kindNames() {
  static const std::vector<std::pair<Kind, std::string>> names = {
      {Kind::TxAdd, "tx_add"},           {Kind::TxJunk, "tx_junk"},       {Kind::Status, "status"},
      {Kind::AccountGet, "account_get"}, {Kind::BlockGet, "block_get"}, {Kind::TxByWallet, "tx_get_by_wallet"},
      {Kind::Big, "big"},           {Kind::BlockWait, "block_wait"}, {Kind::Register, "register"}};
  return names;
}

bool contains(const std::string &text, const char *part) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find(part) != std::string::npos;
}

bool isTransfer(Kind kind) { return kind == Kind::TxAdd || kind == Kind::TxJunk; }

uint32_t requestType(Kind kind) {
  switch (kind) {
  case Kind::TxAdd:
  case Kind::TxJunk:
    return Client::T_REQ_TX_ADD;
  case Kind::Status:
    return Client::T_REQ_STATUS;
  case Kind::AccountGet:
    return Client::T_REQ_ACCOUNT_GET;
  case Kind::BlockGet:
    return Client::T_REQ_BLOCK_GET;
  case Kind::TxByWallet:
    return Client::T_REQ_TX_GET_BY_WALLET;
  case Kind::Big:
    return Client::T_REQ_BLOCK_ADD;
  case Kind::BlockWait:
    return Client::T_REQ_BLOCK_WAIT;
  case Kind::Register:
    return Client::T_REQ_REGISTER;
  }
  return 0;
}

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** An error message with its numbers masked ("account N"), cut to 100 characters: one line per kind. */
std::string errorKind(const std::string &message) {
  std::string out;
  for (size_t i = 0; i < message.size() && out.size() < 100; ++i) {
    if (std::isdigit(static_cast<unsigned char>(message[i]))) {
      out += 'N';
      while (i + 1 < message.size() && std::isdigit(static_cast<unsigned char>(message[i + 1]))) {
        ++i;
      }
    } else {
      out += message[i];
    }
  }
  return out;
}

double msSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

} // namespace

std::string kindName(Kind kind) {
  for (const auto &[k, name] : kindNames()) {
    if (k == kind) {
      return name;
    }
  }
  return "?";
}

std::optional<Kind> kindFromName(const std::string &name) {
  for (const auto &[k, n] : kindNames()) {
    if (n == name) {
      return k;
    }
  }
  return std::nullopt;
}

pp::Roe<Mix> parseMix(const std::string &spec) {
  Mix mix;
  std::stringstream in(spec);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (item.empty()) {
      continue;
    }
    const auto eq = item.find('=');
    const std::string name = item.substr(0, eq);
    const auto kind = kindFromName(name);
    if (!kind) {
      return pp::Error("Unknown request kind '" + name + "' in the mix");
    }
    uint32_t weight = 1;
    if (eq != std::string::npos) {
      try {
        weight = static_cast<uint32_t>(std::stoul(item.substr(eq + 1)));
      } catch (const std::exception &) {
        return pp::Error("Bad weight in the mix: '" + item + "'");
      }
    }
    if (weight > 0) {
      mix.emplace_back(*kind, weight);
    }
  }
  if (mix.empty()) {
    return pp::Error("The mix names no request kind");
  }
  return mix;
}

std::string outcomeName(Outcome outcome) {
  switch (outcome) {
  case Outcome::Taken:
    return "taken";
  case Outcome::Duplicate:
    return "duplicate";
  case Outcome::PoolFull:
    return "pool_full";
  case Outcome::Served:
    return "served";
  case Outcome::Busy:
    return "busy";
  case Outcome::Expired:
    return "expired";
  case Outcome::Timeout:
    return "timeout";
  case Outcome::Unsent:
    return "unsent";
  case Outcome::Refused:
    return "refused";
  }
  return "?";
}

Outcome classify(Kind kind, const Client::Roe<std::string> &reply) {
  if (reply) {
    if (!isTransfer(kind)) {
      return Outcome::Served;
    }
    return contains(reply.value(), "already submitted") ? Outcome::Duplicate : Outcome::Taken;
  }
  switch (reply.error().code) { // the node's own word first (LR007)
  case Client::E_BUSY:
  case Client::E_RATE_LIMITED:
    return Outcome::Busy;
  case Client::E_POOL_FULL:
    return Outcome::PoolFull;
  case Client::E_UPSTREAM_TIMEOUT:
    return Outcome::Timeout;
  case Client::E_EXPIRED:
    return Outcome::Expired;
  default:
    break;
  }
  const std::string &message = reply.error().message;
  if (Client::isTransportError(reply.error().code)) {
    // Sent and not answered in time, or never sent: no link (a dial that timed out or backs off), no
    // channel, no free place to the peer.
    return contains(message, "response timeout") ? Outcome::Timeout : Outcome::Unsent;
  }
  if (contains(message, "timeout") || contains(message, "timed out")) {
    return Outcome::Timeout;
  }
  if (contains(message, "pool full")) {
    return Outcome::PoolFull;
  }
  if (contains(message, "busy")) {
    return Outcome::Busy;
  }
  if (contains(message, "expired")) {
    return Outcome::Expired;
  }
  return Outcome::Refused;
}

double Latencies::quantile(double q) const {
  if (samples_.empty()) {
    return 0;
  }
  std::vector<double> sorted = samples_;
  std::sort(sorted.begin(), sorted.end());
  const double rank = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
  return sorted[static_cast<size_t>(std::llround(rank))];
}

double Latencies::max() const { return samples_.empty() ? 0 : *std::max_element(samples_.begin(), samples_.end()); }

// ============ accounts ============

pp::Roe<void> writeAccounts(const std::string &path, const std::string &networkId,
                            const std::vector<Account> &accounts) {
  Object root;
  root.set("networkId", networkId);
  std::vector<Value> list;
  for (const auto &account : accounts) {
    auto entry = std::make_shared<Object>();
    entry->setUIntForJson("id", account.id);
    entry->set("key", utl::hexEncode(account.privateKey));
    list.emplace_back(std::move(entry));
  }
  root.set("accounts", Object::array(std::move(list)));
  auto text = pp::common::io::valueToJsonString(Value(std::make_shared<Object>(root)), 2);
  if (!text.isOk()) {
    return pp::Error(text.error().message);
  }
  std::ofstream out(path, std::ios::trunc);
  out << text.value() << "\n";
  if (!out) {
    return pp::Error("Cannot write " + path);
  }
  return {};
}

pp::Roe<std::vector<Account>> readAccounts(const std::string &path, std::string &networkId) {
  auto json = utl::loadJsonFile(path);
  if (!json) {
    return pp::Error("Cannot read " + path + ": " + json.error().message);
  }
  const Object &root = json.value();
  networkId = root.getString("networkId").value_or("");
  const auto *list = root.getArray("accounts");
  if (!list) {
    return pp::Error(path + ": no `accounts`");
  }
  std::vector<Account> accounts;
  for (const auto &value : list->elements) {
    const auto *entry = pp::common::asObject(value);
    if (!entry) {
      return pp::Error(path + ": an account is not an object");
    }
    Account account;
    account.id = entry->getNonNegInt("id").value_or(0);
    account.privateKey = utl::hexDecode(entry->getString("key").value_or(""));
    if (account.id == 0 || account.privateKey.size() != utl::kMlDsaPrivateKeyBytes) {
      return pp::Error(path + ": an account needs an id and an ML-DSA-65 private key");
    }
    accounts.push_back(std::move(account));
  }
  return accounts;
}

// ============ transactions ============

namespace {

uint64_t randomId(std::mt19937_64 &rng) {
  return std::uniform_int_distribution<uint64_t>(AccountIds::ID_FIRST_USER, UINT64_MAX)(rng);
}

void setWindow(Ledger::TxIdempotencyWindow &tx, std::mt19937_64 &rng) {
  const int64_t now = nowSeconds();
  tx.idempotentId = std::uniform_int_distribution<uint64_t>(1, UINT64_MAX)(rng);
  tx.validationTsMin = now - 60;
  tx.validationTsMax = now + 3600;
}

/** Sign `record` with each of `keys` for `networkId`. */
pp::Roe<void> sign(Ledger::Record &record, const std::vector<std::string> &keys, const std::string &networkId) {
  const std::string message = record.signingMessage(networkId);
  for (const auto &key : keys) {
    auto signature = utl::mlDsaSign(key, message);
    if (!signature) {
      return pp::Error(signature.error().message);
    }
    record.signatures.push_back(signature.value());
  }
  return {};
}

std::string keyOf(const Ledger::Record &record) { return utl::sha256Raw(utl::binaryPack(record)); }

thread_local std::mt19937_64 tlsRng{std::random_device{}()};

} // namespace

pp::Roe<Ledger::Record> transfer(const Account &from, uint64_t to, uint64_t amount, uint64_t fee,
                                 const std::string &networkId, bool junk) {
  Ledger::TxDefault tx;
  tx.fromWalletId = from.id;
  tx.toWalletId = to;
  tx.amount = amount;
  tx.fee = fee;
  setWindow(tx, tlsRng);
  Ledger::Record record;
  record.type = Ledger::T_DEFAULT;
  record.data = utl::binaryPack(tx);
  if (auto signed_ = sign(record, {from.privateKey}, networkId); !signed_) {
    return signed_.error();
  }
  if (junk) {
    record.signatures.front()[0] = static_cast<char>(record.signatures.front()[0] ^ 0x5a); // decodes, never verifies
  }
  return record;
}

// ============ clients ============

namespace {

/** One client identity: its own AMP runtime (and so its own peer id), dialing the host. */
struct Peer {
  network::LedgerAmpRuntime runtime;
  Client client;
};

pp::Roe<std::unique_ptr<Peer>> connect(const std::string &host) {
  auto keys = utl::mlDsaGenerate();
  if (!keys) {
    return pp::Error("Cannot make a client identity: " + keys.error().message);
  }
  auto config = network::LedgerAmpConfigFromPrivateKey(keys->privateKey, 0);
  if (!config) {
    return pp::Error(config.error().message);
  }
  auto peer = std::make_unique<Peer>();
  if (auto started = peer->runtime.Start(*config); !started) {
    return pp::Error("Cannot start a client: " + started.error().message);
  }
  peer->client.attachAmpTransport(peer->runtime, "remote");
  if (auto dial = peer->client.setAmpPeer("remote", host); !dial) {
    return pp::Error("Cannot dial " + host + ": " + dial.error().message);
  }
  return peer;
}

/** A request planned before the run starts (transfers are signed up front: signing is not the load). */
struct Planned {
  Kind kind;
  std::string payload;
  std::string key; ///< transfers: the record's pool key
  Clock::duration at;
};

struct Sent {
  Clock::time_point at;
  uint64_t tipAtSend{0};
  bool valid{false};
  bool taken{false};
  bool included{false};
};

} // namespace

pp::Roe<RunReport> run(const RunConfig &cfg, const std::vector<Account> &accounts) {
  if (cfg.rate <= 0 || cfg.peers == 0) {
    return pp::Error("rate and peers must be positive");
  }
  const bool transfers = cfg.burst > 0 || std::any_of(cfg.mix.begin(), cfg.mix.end(),
                                                      [](const auto &k) { return isTransfer(k.first); });
  if (transfers && accounts.size() < 2) {
    return pp::Error("Transfers need at least two funded accounts (pp-client load-setup)");
  }

  std::vector<std::unique_ptr<Peer>> peers;
  for (size_t i = 0; i < cfg.peers + 1; ++i) { // the last one follows blocks
    auto peer = connect(cfg.host);
    if (!peer) {
      return peer.error();
    }
    peers.push_back(std::move(peer.value()));
  }
  Peer &watcher = *peers.back();
  auto state = watcher.client.fetchBeaconState();
  if (!state) {
    return pp::Error("Cannot reach " + cfg.host + ": " + state.error().message);
  }
  const std::string networkId = state.value().networkId;
  if (networkId.empty()) {
    return pp::Error(cfg.host + " did not say which network it is on: cannot sign transfers for it");
  }
  std::atomic<uint64_t> tip{state.value().nextBlockId};

  // Plan every request.
  std::mt19937_64 rng{std::random_device{}()};
  uint32_t totalWeight = 0;
  for (const auto &[kind, weight] : cfg.mix) {
    totalWeight += weight;
  }
  auto pickKind = [&]() {
    uint32_t r = std::uniform_int_distribution<uint32_t>(0, totalWeight - 1)(rng);
    for (const auto &[kind, weight] : cfg.mix) {
      if (r < weight) {
        return kind;
      }
      r -= weight;
    }
    return cfg.mix.front().first;
  };
  auto pickAccount = [&]() -> const Account & {
    return accounts[std::uniform_int_distribution<size_t>(0, accounts.size() - 1)(rng)];
  };
  auto planTransfer = [&](Kind kind, Clock::duration at) -> pp::Roe<Planned> {
    const Account &from = pickAccount();
    const Account *to = &pickAccount();
    while (to->id == from.id) {
      to = &pickAccount();
    }
    auto record = transfer(from, to->id, cfg.amount, cfg.fee, networkId, kind == Kind::TxJunk);
    if (!record) {
      return record.error();
    }
    return Planned{kind, utl::binaryPack(record.value()), keyOf(record.value()), at};
  };
  std::vector<Planned> plan;
  const auto total = static_cast<uint64_t>(cfg.rate * static_cast<double>(cfg.duration.count()));
  for (uint64_t i = 0; i < total; ++i) {
    const auto at = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(static_cast<double>(i) / cfg.rate));
    const Kind kind = pickKind();
    if (isTransfer(kind)) {
      auto planned = planTransfer(kind, at);
      if (!planned) {
        return planned.error();
      }
      plan.push_back(std::move(planned.value()));
    } else {
      plan.push_back(Planned{kind, {}, {}, at}); // reads: payload at send time
    }
  }
  for (uint64_t i = 0; i < cfg.burst; ++i) {
    auto planned = planTransfer(Kind::TxAdd, cfg.burstAt);
    if (!planned) {
      return planned.error();
    }
    plan.push_back(std::move(planned.value()));
  }
  std::stable_sort(plan.begin(), plan.end(), [](const Planned &a, const Planned &b) { return a.at < b.at; });

  // Shared with the reply callbacks (AMP io lanes, which may outlive this call) and the watcher.
  struct Shared {
    std::mutex mu;
    std::condition_variable repliesDone;
    RunReport report;
    std::unordered_map<std::string, Sent> sent;
    uint64_t outstanding = 0;
    Clock::time_point lastInclusion = Clock::now();
  };
  auto shared = std::make_shared<Shared>();
  std::mutex &mu = shared->mu;
  RunReport &report = shared->report;
  auto &sent = shared->sent;
  report.firstBlock = tip;

  std::atomic<bool> watching{true};
  std::thread follow([&]() {
    uint64_t next = tip;
    while (watching) {
      auto status = watcher.client.fetchBeaconState();
      if (status) {
        tip = status.value().nextBlockId;
        for (; next < tip && watching; ++next) {
          auto block = watcher.client.fetchBlock(next);
          if (!block) {
            break; // again next round
          }
          std::lock_guard<std::mutex> lock(mu);
          for (const auto &record : block.value().block.records) {
            auto it = sent.find(keyOf(record));
            if (it == sent.end() || it->second.included) {
              continue;
            }
            it->second.included = true;
            if (!it->second.valid) {
              ++report.junkIncluded;
              continue;
            }
            ++report.included;
            shared->lastInclusion = Clock::now();
            report.inclusionMs.add(msSince(it->second.at));
            report.inclusionBlocks.add(static_cast<double>(next + 1 - it->second.tipAtSend));
          }
          report.lastBlock = next;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });

  // Send, open loop.
  const auto start = Clock::now();
  size_t peerIndex = 0;
  for (const auto &planned : plan) {
    std::this_thread::sleep_until(start + planned.at);
    std::string payload = planned.payload;
    switch (planned.kind) {
    case Kind::AccountGet:
      payload = utl::binaryPack(pickAccount().id);
      break;
    case Kind::BlockGet:
      payload = utl::binaryPack(std::uniform_int_distribution<uint64_t>(0, std::max<uint64_t>(tip, 1) - 1)(rng));
      break;
    case Kind::TxByWallet: {
      Client::TxGetByWalletRequest request;
      request.walletId = pickAccount().id;
      payload = utl::binaryPack(request);
      break;
    }
    case Kind::Big: {
      static const std::string big(4 * 1024 * 1024 - 4096, 'b'); // under the RPC's limit, no block
      payload = big;
      break;
    }
    case Kind::BlockWait:
      payload = utl::binaryPack(tip);
      break;
    case Kind::Register: {
      Client::MinerInfo info; // a signature of the right size, so the beacon verifies it
      info.id = pickAccount().id;
      info.issuedAt = nowSeconds();
      info.signatures = {std::string(utl::kMlDsaSignatureBytes, 'r')};
      payload = utl::binaryPack(info.ltsToMeta());
      break;
    }
    default:
      break;
    }
    const auto sentAt = Clock::now();
    {
      std::lock_guard<std::mutex> lock(mu);
      ++report.kinds[planned.kind].sent;
      ++shared->outstanding;
      if (isTransfer(planned.kind)) {
        sent[planned.key] = Sent{sentAt, tip, planned.kind == Kind::TxAdd, false, false};
        report.validSent += planned.kind == Kind::TxAdd ? 1 : 0;
      }
    }
    Peer &peer = *peers[peerIndex++ % cfg.peers];
    peer.client.requestAsync(requestType(planned.kind), payload,
                             [shared, kind = planned.kind, key = planned.key, sentAt](Client::Roe<std::string> reply) {
                               const Outcome outcome = classify(kind, reply);
                               std::lock_guard<std::mutex> lock(shared->mu);
                               auto &k = shared->report.kinds[kind];
                               ++k.outcomes[outcome];
                               k.latencyMs.add(msSince(sentAt));
                               if (!reply) {
                                 ++k.errors[errorKind(reply.error().message)];
                               }
                               if (kind == Kind::TxAdd && (outcome == Outcome::Taken || outcome == Outcome::Duplicate)) {
                                 ++shared->report.taken;
                                 shared->sent[key].taken = true;
                               }
                               if (--shared->outstanding == 0) {
                                 shared->repliesDone.notify_all();
                               }
                             },
                             cfg.timeout);
  }
  report.seconds = std::chrono::duration<double>(Clock::now() - start).count();

  // Every reply (each call ends by its own timeout), then the blocks that include what was taken.
  {
    std::unique_lock<std::mutex> lock(mu);
    shared->repliesDone.wait_for(lock, std::chrono::minutes(2), [&]() { return shared->outstanding == 0; });
  }
  const auto drainEnd = Clock::now() + cfg.drain;
  {
    std::lock_guard<std::mutex> lock(mu);
    shared->lastInclusion = std::max(shared->lastInclusion, Clock::now()); // quiet counts from the last reply
  }
  while (Clock::now() < drainEnd) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (report.included >= report.validSent || Clock::now() - shared->lastInclusion > cfg.quiet) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  watching = false;
  follow.join();
  for (auto &peer : peers) {
    peer->runtime.Stop();
  }
  std::lock_guard<std::mutex> lock(mu);
  for (const auto &[key, entry] : sent) {
    report.includedNotTaken += entry.valid && entry.included && !entry.taken ? 1 : 0;
  }
  return report;
}

// ============ report ============

namespace {

std::string fixed(double v, int digits = 1) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << v;
  return out.str();
}

} // namespace

std::string RunReport::text() const {
  std::ostringstream out;
  uint64_t sentTotal = 0;
  for (const auto &[kind, k] : kinds) {
    sentTotal += k.sent;
  }
  out << "Sent " << sentTotal << " requests in " << fixed(seconds) << " s ("
      << fixed(seconds > 0 ? static_cast<double>(sentTotal) / seconds : 0) << "/s)\n";
  for (const auto &[kind, k] : kinds) {
    out << "  " << kindName(kind) << ": sent " << k.sent;
    for (const auto &[outcome, n] : k.outcomes) {
      out << ", " << outcomeName(outcome) << " " << n;
    }
    out << "; reply ms p50 " << fixed(k.latencyMs.quantile(0.5)) << " p99 " << fixed(k.latencyMs.quantile(0.99))
        << " max " << fixed(k.latencyMs.max()) << "\n";
  }
  for (const auto &[kind, k] : kinds) {
    std::vector<std::pair<uint64_t, std::string>> top;
    for (const auto &[message, n] : k.errors) {
      top.emplace_back(n, message);
    }
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 5; ++i) {
      out << "    " << kindName(kind) << " error x" << top[i].first << ": " << top[i].second << "\n";
    }
  }
  out << "Transfers sent " << validSent << ", taken " << taken << ", included " << included << " (blocks "
      << firstBlock << "-" << lastBlock << ")";
  if (included > 0) {
    out << "; inclusion s p50 " << fixed(inclusionMs.quantile(0.5) / 1000) << " p99 "
        << fixed(inclusionMs.quantile(0.99) / 1000) << ", blocks p50 " << fixed(inclusionBlocks.quantile(0.5), 0)
        << " p99 " << fixed(inclusionBlocks.quantile(0.99), 0) << " max " << fixed(inclusionBlocks.max(), 0);
  }
  out << "\n";
  if (includedNotTaken > 0) {
    out << "Included although the reply was an error: " << includedNotTaken << " (a client retrying pays twice)\n";
  }
  if (junkIncluded > 0) {
    out << "Junk included: " << junkIncluded << " (must be 0)\n";
  }
  return out.str();
}

Object RunReport::json() const {
  Object root;
  root.set("seconds", seconds);
  std::vector<Value> list;
  for (const auto &[kind, k] : kinds) {
    auto entry = std::make_shared<Object>();
    entry->set("kind", kindName(kind));
    entry->setUIntForJson("sent", k.sent);
    for (const auto &[outcome, n] : k.outcomes) {
      entry->setUIntForJson(outcomeName(outcome), n);
    }
    entry->set("replyMsP50", k.latencyMs.quantile(0.5));
    entry->set("replyMsP99", k.latencyMs.quantile(0.99));
    entry->set("replyMsMax", k.latencyMs.max());
    list.emplace_back(std::move(entry));
  }
  root.set("kinds", Object::array(std::move(list)));
  root.setUIntForJson("validSent", validSent);
  root.setUIntForJson("taken", taken);
  root.setUIntForJson("included", included);
  root.setUIntForJson("includedNotTaken", includedNotTaken);
  root.setUIntForJson("junkIncluded", junkIncluded);
  root.setUIntForJson("firstBlock", firstBlock);
  root.setUIntForJson("lastBlock", lastBlock);
  root.set("inclusionSecondsP50", inclusionMs.quantile(0.5) / 1000);
  root.set("inclusionSecondsP99", inclusionMs.quantile(0.99) / 1000);
  root.set("inclusionBlocksP50", inclusionBlocks.quantile(0.5));
  root.set("inclusionBlocksP99", inclusionBlocks.quantile(0.99));
  root.set("inclusionBlocksMax", inclusionBlocks.max());
  return root;
}

// ============ setup ============

pp::Roe<std::vector<Account>> setup(const SetupConfig &cfg, std::string &networkId) {
  if (cfg.fromKeys.empty()) {
    return pp::Error("The funding account's keys are required");
  }
  auto connected = connect(cfg.host);
  if (!connected) {
    return connected.error();
  }
  Peer &peer = *connected.value();
  auto state = peer.client.fetchBeaconState();
  if (!state) {
    return pp::Error("Cannot reach " + cfg.host + ": " + state.error().message);
  }
  networkId = state.value().networkId;
  if (networkId.empty()) {
    return pp::Error(cfg.host + " did not say which network it is on: cannot sign for it");
  }

  std::mt19937_64 rng{std::random_device{}()};
  std::vector<Account> wanted;
  std::vector<std::string> records;
  for (size_t i = 0; i < cfg.accounts; ++i) {
    auto keys = utl::mlDsaGenerate();
    if (!keys) {
      return pp::Error(keys.error().message);
    }
    Client::UserAccount account;
    account.wallet.publicKeys = {keys->publicKey};
    account.wallet.minSignatures = 1;
    account.wallet.keyType = Crypto::TK_ML_DSA_65;
    account.wallet.mBalances[AccountIds::ID_GENESIS] = static_cast<int64_t>(cfg.amount);
    account.meta = AccountAttachment::emptySerialized();
    Ledger::TxNewUser tx;
    tx.fromWalletId = cfg.from;
    tx.toWalletId = randomId(rng);
    tx.amount = cfg.amount;
    tx.fee = cfg.fee;
    tx.meta = account.ltsToString();
    setWindow(tx, rng);
    Ledger::Record record;
    record.type = Ledger::T_NEW_USER;
    record.data = utl::binaryPack(tx);
    if (auto signed_ = sign(record, cfg.fromKeys, networkId); !signed_) {
      return signed_.error();
    }
    wanted.push_back(Account{tx.toWalletId, keys->privateKey});
    records.push_back(utl::binaryPack(record));
  }

  // Submit, retrying what a full queue or pool turned away.
  const auto deadline = Clock::now() + cfg.timeout;
  std::vector<size_t> pending(records.size());
  for (size_t i = 0; i < pending.size(); ++i) {
    pending[i] = i;
  }
  while (!pending.empty() && Clock::now() < deadline) {
    std::vector<size_t> retry;
    for (size_t i : pending) {
      std::promise<Client::Roe<std::string>> done;
      auto result = done.get_future();
      peer.client.requestAsync(Client::T_REQ_TX_ADD, records[i],
                               [&done](Client::Roe<std::string> r) { done.set_value(std::move(r)); });
      const Outcome outcome = classify(Kind::TxAdd, result.get());
      if (outcome != Outcome::Taken && outcome != Outcome::Duplicate) {
        retry.push_back(i);
      }
    }
    pending = std::move(retry);
    if (!pending.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // Wait for the accounts to exist.
  std::vector<Account> created;
  std::set<size_t> missing;
  for (size_t i = 0; i < wanted.size(); ++i) {
    missing.insert(i);
  }
  while (!missing.empty() && Clock::now() < deadline) {
    for (auto it = missing.begin(); it != missing.end();) {
      if (peer.client.fetchUserAccount(wanted[*it].id)) {
        created.push_back(wanted[*it]);
        it = missing.erase(it);
      } else {
        ++it;
      }
    }
    if (!missing.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  peer.runtime.Stop();
  return created;
}

} // namespace pp::load
