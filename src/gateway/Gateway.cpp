#include "gateway/Gateway.h"

#include "chain/BeaconCertTxHandler.h"
#include "chain/BlockValidation.h"
#include "ledger/chain/ClientVerify.h"
#include "ledger/client/AccountIds.h"
#include "ledger/lib/common/BinaryPack.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>

namespace pp::gateway {

namespace {

/** A journal entry's bytes, at most (a certificate record is the largest). */
constexpr uint32_t kMaxRecordBytes = 16 * 1024 * 1024;

std::optional<BlockChainConfig> configOfGenesis(const Ledger::Block &genesis) {
  if (genesis.records.empty() || genesis.records[0].type != Ledger::T_GENESIS) {
    return std::nullopt;
  }
  auto tx = utl::binaryUnpack<Ledger::TxGenesis>(genesis.records[0].data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return std::nullopt;
  }
  return gm.config;
}

std::optional<uint64_t> finalityOfConfigUpdate(const Ledger::Record &record) {
  if (record.type != Ledger::T_CONFIG) {
    return std::nullopt;
  }
  auto tx = utl::binaryUnpack<Ledger::TxConfig>(record.data);
  GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    return std::nullopt;
  }
  return gm.config.finalitySeconds;
}

/** One journal entry: its size (4 bytes, little-endian), then its bytes. The bytes written, or 0. */
uint64_t writeEntry(std::ostream &out, const std::string &bytes) {
  const auto size = static_cast<uint32_t>(bytes.size());
  const char header[4] = {static_cast<char>(size & 0xff), static_cast<char>((size >> 8) & 0xff),
                          static_cast<char>((size >> 16) & 0xff), static_cast<char>((size >> 24) & 0xff)};
  out.write(header, 4);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out ? 4 + bytes.size() : 0;
}

} // namespace

const char *Event::kindName(uint8_t kind) {
  switch (kind) {
  case Certificate:
    return "certificate";
  case Rewind:
    return "rewind";
  case Equivocation:
    return "equivocation";
  case Forked:
    return "forked";
  case Unexplained:
    return "unexplained";
  default:
    return "unknown";
  }
}

/**
 * One change to what the gateway holds, appended to `journal.bin` before it is applied; open()
 * replays them. Fields by kind (unused ones stay zero).
 */
struct Gateway::Record {
  enum Kind : uint8_t {
    Start = 1,         ///< following from the next Block (forget the blocks held)
    Block = 2,         ///< a: height, b: slot, c: epoch, d: finalAt, s1: hash, s2: state root
    Step = 3,          ///< s1: AuthorityStep
    Watch = 5,         ///< a: account
    Unwatch = 6,       ///< a: account
    Touch = 7,         ///< a: account, b: a block whose records name it
    Balance = 8,       ///< a: account, b: from height, c: 1 known, d: 1 present, balances
    Proven = 9,        ///< a: account, b: proven up to (touches up to it settled)
    Certificate = 10,  ///< s1: the T_BEACON_CERT record held
    Equivocation = 11, ///< s1: the evidence held (empty: cleared)
    Rewind = 12,       ///< a: the last block kept
    Forked = 13,       ///< a: the height a certificate vouches for another block at
    Event = 14,        ///< s1: Event
    Prune = 15,        ///< a: the oldest block kept (final ones below it forgotten)
  };

  uint8_t kind{0};
  uint64_t a{0};
  uint64_t b{0};
  uint64_t c{0};
  uint64_t d{0};
  std::string s1;
  std::string s2;
  std::map<uint64_t, int64_t> balances;

  template <typename Archive> void serialize(Archive &ar) { ar & kind & a & b & c & d & s1 & s2 & balances; }
};

Gateway::Gateway(Source &source, Options options)
    : source_(source), options_(std::move(options)), log_(logging::getLogger("Gateway")) {}

Gateway::~Gateway() = default;

// ── journal ─────────────────────────────────────────────────────────────────

Gateway::Roe<void> Gateway::open() {
  std::lock_guard lock(mutex_);
  if (options_.genesisHash.size() != 32) {
    return Error(kBadRequest, "The genesis hash must be 32 bytes (64 hex digits)");
  }
  std::error_code ec;
  std::filesystem::create_directories(options_.dataDir, ec);
  if (ec) {
    return Error(kUnavailable, "Cannot create " + options_.dataDir.string() + ": " + ec.message());
  }
  if (auto loaded = loadJournal(); !loaded) {
    return loaded;
  }
  journal_.open(options_.dataDir / "journal.bin", std::ios::binary | std::ios::app);
  if (!journal_) {
    return Error(kUnavailable, "Cannot open the journal in " + options_.dataDir.string());
  }
  journalBytes_ = std::filesystem::file_size(options_.dataDir / "journal.bin", ec);
  compactedBytes_ = journalBytes_;
  return {};
}

Gateway::Roe<void> Gateway::loadJournal() {
  const auto path = options_.dataDir / "journal.bin";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {}; // a new data directory
  }
  const auto pinned = options_.dataDir / "genesis.hash";
  {
    std::ifstream hashIn(pinned, std::ios::binary);
    std::string held((std::istreambuf_iterator<char>(hashIn)), std::istreambuf_iterator<char>());
    if (!held.empty() && held != options_.genesisHash) {
      return Error(kBadRequest, "This data directory follows another chain (genesis " + utl::hexEncode(held) + ")");
    }
  }
  uint64_t good = 0;
  size_t count = 0;
  for (;;) {
    unsigned char header[4];
    if (!in.read(reinterpret_cast<char *>(header), 4)) {
      break;
    }
    const uint32_t size = static_cast<uint32_t>(header[0]) | static_cast<uint32_t>(header[1]) << 8 |
                          static_cast<uint32_t>(header[2]) << 16 | static_cast<uint32_t>(header[3]) << 24;
    if (size > kMaxRecordBytes) {
      break;
    }
    std::string bytes(size, '\0');
    if (!in.read(bytes.data(), size)) {
      break;
    }
    auto record = utl::binaryUnpack<Record>(bytes);
    if (!record) {
      break;
    }
    apply(record.value());
    good += 4 + size;
    ++count;
  }
  in.close();
  // A tail cut short (a crash mid-write) is dropped: the change it began never happened.
  if (std::filesystem::file_size(path) != good) {
    log_.warning << "Journal: dropping an incomplete tail after " << count << " entries";
    std::filesystem::resize_file(path, good);
  }
  log_.info << "Journal: " << count << " entries; " << facts_.size() << " blocks held, " << accounts_.size()
            << " accounts watched";
  return {};
}

Gateway::Roe<void> Gateway::append(const Record &record) {
  if (!journal_.is_open()) {
    return Error(kUnavailable, "The journal is not open");
  }
  const uint64_t written = writeEntry(journal_, utl::binaryPack(record));
  if (written == 0 || !journal_.flush()) {
    return Error(kUnavailable, "Cannot write the journal");
  }
  journalBytes_ += written;
  apply(record);
  return {};
}

void Gateway::apply(const Record &record) {
  switch (record.kind) {
  case Record::Start:
    facts_.clear();
    finalized_.reset();
    break;
  case Record::Block:
    facts_.push_back(Facts{record.a, record.s1, record.b, record.c, record.s2, static_cast<int64_t>(record.d)});
    break;
  case Record::Step:
    if (auto step = utl::binaryUnpack<Client::AuthorityStep>(record.s1)) {
      steps_.push_back(step.value());
    }
    break;
  case Record::Watch:
    accounts_.try_emplace(record.a);
    break;
  case Record::Unwatch:
    accounts_.erase(record.a);
    break;
  case Record::Touch:
    if (auto it = accounts_.find(record.a); it != accounts_.end()) {
      it->second.touches.insert(record.b);
    }
    break;
  case Record::Balance:
    if (auto it = accounts_.find(record.a); it != accounts_.end()) {
      it->second.entries[record.b] = Entry{record.c != 0, record.d != 0, record.balances};
    }
    break;
  case Record::Proven:
    if (auto it = accounts_.find(record.a); it != accounts_.end()) {
      it->second.provenUpTo = record.b;
      auto &touches = it->second.touches;
      touches.erase(touches.begin(), touches.upper_bound(record.b));
    }
    break;
  case Record::Certificate: {
    if (record.s1.empty()) { // the one held no longer stands
      seen_.reset();
      seenRecord_.reset();
      break;
    }
    auto record_ = utl::binaryUnpack<Ledger::Record>(record.s1);
    if (!record_) {
      break;
    }
    auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record_.value().data);
    if (!tx) {
      break;
    }
    if (auto cert = BeaconCertTxHandler::decodeCertificate(tx.value())) {
      seen_ = cert.value();
      seenRecord_ = record_.value();
    }
    break;
  }
  case Record::Equivocation:
    if (record.s1.empty()) {
      equivocation_.reset();
    } else if (Client::Equivocation evidence; evidence.ltsFromString(record.s1)) {
      equivocation_ = evidence;
    }
    break;
  case Record::Rewind: {
    const uint64_t kept = record.a;
    while (!facts_.empty() && facts_.back().height > kept) {
      facts_.pop_back();
    }
    std::erase_if(steps_, [kept](const Client::AuthorityStep &step) { return step.blockId > kept; });
    if (lastTip_ && lastTip_->height > kept) {
      lastTip_.reset();
    }
    if (finalized_ && *finalized_ > kept) {
      finalized_ = kept;
    }
    for (auto &[id, account] : accounts_) {
      account.entries.erase(account.entries.upper_bound(kept), account.entries.end());
      account.touches.erase(account.touches.upper_bound(kept), account.touches.end());
      if (account.entries.empty()) {
        account.provenUpTo.reset(); // proven only above the cutover: as if just watched
      } else if (account.provenUpTo && *account.provenUpTo > kept) {
        account.provenUpTo = kept;
      }
    }
    break;
  }
  case Record::Forked:
    forkedAt_ = record.a;
    break;
  case Record::Prune:
    if (!facts_.empty() && record.a > facts_.front().height && record.a <= facts_.back().height) {
      facts_.erase(facts_.begin(), facts_.begin() + static_cast<std::ptrdiff_t>(record.a - facts_.front().height));
      // An account keeps its last entry at or below the oldest block kept: its balances there.
      for (auto &[id, account] : accounts_) {
        auto last = account.entries.upper_bound(record.a);
        if (last != account.entries.begin()) {
          account.entries.erase(account.entries.begin(), std::prev(last));
        }
      }
    }
    break;
  case Record::Event:
    if (auto event = utl::binaryUnpack<Event>(record.s1)) {
      nextSeq_ = std::max(nextSeq_, event.value().seq + 1);
      events_.push_back(event.value());
    }
    break;
  default:
    break;
  }
  if (record.kind == Record::Step || record.kind == Record::Start || record.kind == Record::Rewind ||
      record.kind == Record::Prune ||
      (record.kind == Record::Block && facts_.size() == 1)) {
    rebuildFinalityHistory();
  }
}

void Gateway::emit(Event event) {
  event.seq = nextSeq_;
  Record record;
  record.kind = Record::Event;
  record.s1 = utl::binaryPack(event);
  if (auto written = append(record); !written) {
    log_.error << "Event " << Event::kindName(event.kind) << " not recorded: " << written.error().message;
  }
  log_.warning << "Event " << event.seq << ": " << Event::kindName(event.kind) << " at height " << event.height;
}

std::vector<Gateway::Record> Gateway::snapshot() const {
  std::vector<Record> out;
  auto add = [&out](Record record) { out.push_back(std::move(record)); };
  Record start;
  start.kind = Record::Start;
  add(start);
  for (const auto &step : steps_) {
    Record record;
    record.kind = Record::Step;
    record.s1 = utl::binaryPack(step);
    add(record);
  }
  for (const auto &facts : facts_) {
    Record record;
    record.kind = Record::Block;
    record.a = facts.height;
    record.b = facts.slot;
    record.c = facts.epoch;
    record.s1 = facts.hash;
    record.s2 = facts.stateRoot;
    record.d = static_cast<uint64_t>(facts.finalAt);
    add(record);
  }
  if (seenRecord_) {
    Record record;
    record.kind = Record::Certificate;
    record.s1 = utl::binaryPack(*seenRecord_);
    add(record);
  }
  if (equivocation_) {
    Record record;
    record.kind = Record::Equivocation;
    record.s1 = equivocation_->ltsToString();
    add(record);
  }
  if (forkedAt_) {
    Record record;
    record.kind = Record::Forked;
    record.a = *forkedAt_;
    add(record);
  }
  for (const auto &[id, account] : accounts_) {
    Record watching;
    watching.kind = Record::Watch;
    watching.a = id;
    add(watching);
    for (const auto &[height, entry] : account.entries) {
      Record record;
      record.kind = Record::Balance;
      record.a = id;
      record.b = height;
      record.c = entry.known ? 1 : 0;
      record.d = entry.present ? 1 : 0;
      record.balances = entry.balances;
      add(record);
    }
    if (account.provenUpTo) {
      Record record;
      record.kind = Record::Proven;
      record.a = id;
      record.b = *account.provenUpTo;
      add(record);
    }
    for (uint64_t height : account.touches) {
      Record record;
      record.kind = Record::Touch;
      record.a = id;
      record.b = height;
      add(record);
    }
  }
  for (const auto &event : events_) {
    Record record;
    record.kind = Record::Event;
    record.s1 = utl::binaryPack(event);
    add(record);
  }
  return out;
}

Gateway::Roe<void> Gateway::compact() {
  const auto path = options_.dataDir / "journal.bin";
  const auto fresh = options_.dataDir / "journal.bin.new";
  uint64_t bytes = 0;
  bool ok = true;
  {
    std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
    for (const auto &record : snapshot()) {
      const uint64_t written = ok ? writeEntry(out, utl::binaryPack(record)) : 0;
      ok = written != 0;
      bytes += written;
    }
    ok = ok && out.flush();
  }
  std::error_code ec;
  if (!ok) {
    std::filesystem::remove(fresh, ec);
    return Error(kUnavailable, "Cannot write " + fresh.string());
  }
  // Closed before it is replaced: Windows does not replace an open file.
  journal_.close();
  std::filesystem::rename(fresh, path, ec);
  journal_.clear();
  journal_.open(path, std::ios::binary | std::ios::app);
  if (ec || !journal_) {
    return Error(kUnavailable, "Cannot replace the journal: " + ec.message());
  }
  log_.info << "Journal: rewritten, " << journalBytes_ << " bytes to " << bytes << "; blocks from "
            << facts_.front().height;
  journalBytes_ = bytes;
  compactedBytes_ = bytes;
  return {};
}

// ── following the chain ─────────────────────────────────────────────────────

Gateway::Roe<void> Gateway::poll() {
  if (!genesis_) {
    auto genesis = source_.block(0);
    if (!genesis) {
      return Error(kUnavailable, "Block 0: " + genesis.error().message);
    }
    if (auto checked = verify::checkGenesis(genesis.value(), options_.genesisHash); !checked) {
      return Error(kUnavailable, "Block 0 is not the pinned genesis: " + checked.error().message);
    }
    auto config = configOfGenesis(genesis.value().block);
    if (!config || config->slotsPerEpoch == 0) {
      return Error(kUnavailable, "Block 0's config is unreadable");
    }
    std::lock_guard lock(mutex_);
    genesis_ = genesis.value();
    config_ = *config;
    rebuildFinalityHistory();
    if (std::ofstream out(options_.dataDir / "genesis.hash", std::ios::binary | std::ios::trunc); out) {
      out << options_.genesisHash;
    }
    if (!facts_.empty()) {
      auto trail = rebuildTrail();
      if (!trail) {
        genesis_.reset();
        return trail;
      }
    }
  }
  if (!finalized_ && !facts_.empty()) {
    // Resumed from the journal: what is final already, before anything the node says now (a rewind
    // is measured against it).
    updateFinalized();
  }
  if (facts_.empty()) {
    if (auto started = bootstrap(); !started) {
      return started;
    }
  }
  // The node may move on while its accounts are proven: a proof of a block not verified here yet is
  // taken once that block is (a round or two more).
  for (int round = 0; round < 3; ++round) {
    auto status = source_.status();
    if (!status) {
      return Error(kUnavailable, "STATUS: " + status.error().message);
    }
    const auto &state = status.value();
    if (!state.networkId.empty() && state.networkId != config_.networkId) {
      return Error(kUnavailable, "The node is on network '" + state.networkId + "', not '" + config_.networkId + "'");
    }
    if (state.certificate) {
      if (auto offered = offerCertificate(*state.certificate); !offered) {
        log_.warning << "The node's certificate: " << offered.error().message;
      }
    }
    if (state.equivocation) {
      if (auto offered = offerEquivocation(*state.equivocation); !offered) {
        log_.warning << "The node's equivocation evidence: " << offered.error().message;
      }
    }
    if (forkedAt_) {
      return Error(kUnavailable, haltedReason());
    }
    if (state.signedTip) {
      if (auto advanced = advance(*state.signedTip); !advanced) {
        return advanced;
      }
    }
    auto proven = proveAccounts();
    if (!proven) {
      return proven.error();
    }
    if (!proven.value()) {
      break;
    }
  }
  updateFinalized();
  std::lock_guard lock(mutex_);
  // Forget final blocks past the retention, keeping the newest of them as the oldest held.
  if (finalized_ && !facts_.empty()) {
    const int64_t horizon = now() - options_.keepFinalSeconds;
    uint64_t keepFrom = facts_.front().height;
    for (const auto &facts : facts_) {
      if (facts.height > *finalized_ || slotTime(facts.slot) >= horizon) {
        break;
      }
      keepFrom = facts.height;
    }
    if (keepFrom > facts_.front().height) {
      Record prune;
      prune.kind = Record::Prune;
      prune.a = keepFrom;
      if (auto written = append(prune); !written) {
        log_.error << "Journal: " << written.error().message;
      }
    }
  }
  if (journalBytes_ > options_.compactAfterBytes && journalBytes_ > 2 * compactedBytes_) {
    if (auto compacted = compact(); !compacted) {
      log_.error << "Journal: " << compacted.error().message;
    }
  }
  return {};
}

Gateway::Roe<void> Gateway::bootstrap() {
  auto status = source_.status();
  if (!status) {
    return Error(kUnavailable, "STATUS: " + status.error().message);
  }
  if (!status.value().signedTip) {
    return Error(kUnavailable, "The node has no signed tip yet");
  }
  const Client::SignedTip signedTip = *status.value().signedTip;
  auto tipBlock = source_.block(signedTip.height);
  if (!tipBlock) {
    return Error(kUnavailable, "Block " + std::to_string(signedTip.height) + ": " + tipBlock.error().message);
  }
  auto proof = source_.authorityProof(signedTip.height);
  if (!proof) {
    return Error(kUnavailable, "Authority proof: " + proof.error().message);
  }
  auto trail = verify::authorityAt(*genesis_, options_.genesisHash, proof.value(), signedTip.height,
                                   tipBlock.value().block.epoch);
  if (!trail) {
    return Error(kUnavailable, "Authority: " + trail.error().message);
  }
  // A later certificate the node holds bounds its tip: an old generation's past the cutover is refused.
  std::optional<Ledger::Record> held;
  if (status.value().certificate && trail.value().acceptOutOfBand(*status.value().certificate)) {
    held = status.value().certificate;
  }
  if (auto under = verify::checkBlocksUnderTip(signedTip, {tipBlock.value()}); !under) {
    return Error(kUnavailable, under.error().message);
  }
  if (auto verified = trail.value().verifyTip(signedTip); !verified) {
    return Error(kUnavailable, "Signed tip: " + verified.error().message);
  }
  std::lock_guard lock(mutex_);
  Record start;
  start.kind = Record::Start;
  if (auto written = append(start); !written) {
    return written;
  }
  for (const auto &step : proof.value().steps) {
    Record record;
    record.kind = Record::Step;
    record.s1 = utl::binaryPack(step);
    if (auto written = append(record); !written) {
      return written;
    }
  }
  if (held) {
    Record record;
    record.kind = Record::Certificate;
    record.s1 = utl::binaryPack(*held);
    if (auto written = append(record); !written) {
      return written;
    }
  }
  const auto &block = tipBlock.value();
  Record facts;
  facts.kind = Record::Block;
  facts.a = block.block.index;
  facts.b = block.block.slot;
  facts.c = block.block.epoch;
  facts.s1 = block.hash;
  facts.s2 = block.block.stateRoot;
  facts.d = static_cast<uint64_t>(block.block.finalAt);
  if (auto written = append(facts); !written) {
    return written;
  }
  lastTip_ = signedTip;
  trail_ = std::move(trail.value());
  log_.info << "Following from block " << signedTip.height << " (generation " << signedTip.generation << ")";
  return {};
}

Gateway::Roe<void> Gateway::rebuildTrail() {
  const Facts &top = facts_.back();
  Client::AuthorityProof proof;
  proof.steps = steps_;
  auto trail = AuthorityTrail::replayProof(genesis_->block, proof, top.height, top.epoch);
  if (!trail) {
    return Error(kUnavailable, "Replaying the authority held: " + trail.error().message);
  }
  if (auto stands = heldCertificateStands(trail.value()); !stands) {
    if (auto dropped = dropHeldCertificate(stands.error().message); !dropped) {
      return dropped;
    }
  } else if (seenRecord_) {
    (void)trail.value().acceptOutOfBand(*seenRecord_); // checked just now: it bounds the tips again
  }
  trail.value().setEquivocation(equivocation_);
  trail_ = std::move(trail.value());
  return {};
}

Gateway::Roe<void> Gateway::heldCertificateStands(const AuthorityTrail &trail) const {
  if (!seen_ || !seenRecord_) {
    return {};
  }
  if (seen_->generation <= trail.beacon().generation) {
    return Error(kUnavailable, "generation " + std::to_string(seen_->generation) + " is in force on chain now");
  }
  AuthorityTrail probe = trail;
  probe.setSeenCertificate(std::nullopt);
  if (auto accepted = probe.acceptOutOfBand(*seenRecord_); !accepted) {
    return Error(kUnavailable, accepted.error().message);
  }
  return {};
}

Gateway::Roe<void> Gateway::dropHeldCertificate(const std::string &why) {
  log_.warning << "Dropping the beacon certificate held (generation " << (seen_ ? seen_->generation : 0)
               << "): " << why;
  Record cleared;
  cleared.kind = Record::Certificate; // empty: none held
  return append(cleared);
}

Gateway::Roe<void> Gateway::offerCertificate(const Ledger::Record &record) {
  if (record.type != Ledger::T_BEACON_CERT) {
    return Error(kUnavailable, "Not a beacon certificate");
  }
  auto tx = utl::binaryUnpack<Ledger::TxBeaconCert>(record.data);
  if (!tx) {
    return Error(kUnavailable, "Unreadable beacon certificate");
  }
  auto cert = BeaconCertTxHandler::decodeCertificate(tx.value());
  if (!cert) {
    return Error(kUnavailable, cert.error().message);
  }
  // Order first, as a node does: only one that would be kept is worth its signature checks.
  const uint64_t generation = cert.value().generation;
  if (generation <= trail_->beacon().generation || (seen_ && generation < seen_->generation)) {
    return {};
  }
  if (seen_ && generation == seen_->generation) {
    if (cert.value().ltsToString() == seen_->ltsToString()) {
      return {};
    }
    // As a node: the one held stays while the authority in force would still take it; else (its
    // admin set replaced, a recovery pending) it goes and this one is checked in its place.
    auto stands = heldCertificateStands(*trail_);
    if (stands) {
      log_.warning << "Another admin-signed certificate for generation " << generation
                   << " than the one held: keeping the one held";
      return {};
    }
    std::lock_guard lock(mutex_);
    if (auto dropped = dropHeldCertificate(stands.error().message); !dropped) {
      return dropped;
    }
    trail_->setSeenCertificate(std::nullopt);
  }
  {
    std::lock_guard lock(mutex_);
    if (auto accepted = trail_->acceptOutOfBand(record); !accepted) {
      return Error(kUnavailable, accepted.error().message);
    }
    Record held;
    held.kind = Record::Certificate;
    held.s1 = utl::binaryPack(record);
    if (auto written = append(held); !written) {
      return written;
    }
    Event event;
    event.kind = Event::Certificate;
    event.generation = generation;
    event.height = cert.value().cutoverHeight;
    emit(event);
    refreshEquivocation();
  }
  if (cert.value().hasCutover() && cert.value().cutoverHeight < facts_.back().height) {
    return rewindTo(cert.value());
  }
  return {};
}

Gateway::Roe<void> Gateway::rewindTo(const BeaconCertificate &cert) {
  const uint64_t kept = cert.cutoverHeight;
  const uint64_t top = facts_.back().height;
  const uint64_t base = facts_.front().height;
  if (kept >= base && factsAt(kept)->hash != cert.cutoverHash) {
    std::lock_guard lock(mutex_);
    Record forked;
    forked.kind = Record::Forked;
    forked.a = kept;
    if (auto written = append(forked); !written) {
      return written;
    }
    Event event;
    event.kind = Event::Forked;
    event.generation = cert.generation;
    event.height = kept;
    emit(event);
    return Error(kUnavailable, haltedReason());
  }
  // Below the start: the block the admins vouch for becomes the start.
  std::optional<Ledger::ChainNode> cutover;
  if (kept < base) {
    auto block = source_.block(kept);
    if (!block) {
      return Error(kUnavailable, "Block " + std::to_string(kept) + ": " + block.error().message);
    }
    if (block.value().block.index != kept || chain_block::calculateBlockHash(block.value().block) != block.value().hash ||
        block.value().hash != cert.cutoverHash) {
      return Error(kUnavailable, "Block " + std::to_string(kept) + " from the node is not the one the certificate vouches for");
    }
    cutover = block.value();
  }
  std::lock_guard lock(mutex_);
  const bool crossed = finalized_ && *finalized_ > kept;
  Record rewind;
  rewind.kind = Record::Rewind;
  rewind.a = kept;
  if (auto written = append(rewind); !written) {
    return written;
  }
  if (cutover) {
    Record start;
    start.kind = Record::Start;
    Record facts;
    facts.kind = Record::Block;
    facts.a = cutover->block.index;
    facts.b = cutover->block.slot;
    facts.c = cutover->block.epoch;
    facts.s1 = cutover->hash;
    facts.s2 = cutover->block.stateRoot;
    facts.d = static_cast<uint64_t>(cutover->block.finalAt);
    for (const auto *record : {&start, &facts}) {
      if (auto written = append(*record); !written) {
        return written;
      }
    }
  }
  Event event;
  event.kind = Event::Rewind;
  event.generation = cert.generation;
  event.height = kept;
  event.droppedUpTo = top;
  event.crossedFinality = crossed;
  emit(event);
  lastSweep_ = 0; // prove every watched account again on the new chain
  return rebuildTrail();
}

Gateway::Roe<void> Gateway::offerEquivocation(const Client::Equivocation &evidence) {
  if (evidence.networkId() != config_.networkId) {
    return Error(kUnavailable, "Equivocation evidence is for network '" + evidence.networkId() + "'");
  }
  const uint64_t generation = evidence.generation();
  const auto &inForce = trail_->beacon();
  if (generation < inForce.generation || (seen_ && generation < seen_->generation) || equivocation_) {
    return {};
  }
  const std::string *publicKey = generation == inForce.generation ? &inForce.publicKey
                                 : seen_ && generation == seen_->generation ? &seen_->publicKey
                                                                             : nullptr;
  if (!publicKey) {
    return Error(kUnavailable, "Equivocation evidence names generation " + std::to_string(generation) +
                                   ", which no certificate here names");
  }
  if (!evidence.verify(*publicKey)) {
    return Error(kUnavailable, "Equivocation evidence is not two blocks at one height signed by generation " +
                                   std::to_string(generation));
  }
  std::lock_guard lock(mutex_);
  Record held;
  held.kind = Record::Equivocation;
  held.s1 = evidence.ltsToString();
  if (auto written = append(held); !written) {
    return written;
  }
  trail_->setEquivocation(evidence);
  Event event;
  event.kind = Event::Equivocation;
  event.generation = generation;
  event.height = evidence.height();
  emit(event);
  return {};
}

void Gateway::refreshEquivocation() {
  if (!equivocation_) {
    return;
  }
  const uint64_t generation = equivocation_->generation();
  if (generation < trail_->beacon().generation || (seen_ && generation < seen_->generation)) {
    Record cleared;
    cleared.kind = Record::Equivocation;
    if (auto written = append(cleared); !written) {
      log_.error << written.error().message;
      return;
    }
    trail_->setEquivocation(std::nullopt);
    log_.info << "Generation " << generation << " is superseded: following past its equivocation";
  }
}

Gateway::Roe<void> Gateway::advance(const Client::SignedTip &signedTip) {
  const Facts top = facts_.back();
  if (signedTip.height < top.height) {
    return {}; // the node is behind
  }
  if (signedTip.height == top.height) {
    // Another block signed at our height: if both verify, the beacon equivocated.
    if (signedTip.blockHash != top.hash && lastTip_ && trail_->verifyTip(signedTip)) {
      if (auto evidence = Client::Equivocation::of(*lastTip_, signedTip)) {
        return offerEquivocation(*evidence);
      }
    }
    return {};
  }
  // What the evidence held refuses, before fetching anything. (The certificate held bounds the tip
  // in verifyTip below, once the blocks' authority changes have re-checked it: they may void it.)
  if (equivocation_ && signedTip.generation == equivocation_->generation() && signedTip.height > equivocation_->height()) {
    return Error(kUnavailable, haltedReason());
  }
  std::vector<uint64_t> watching;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[id, account] : accounts_) {
      watching.push_back(id);
    }
  }
  // Bottom up from the tip held: each block hashes to its hash and links to the one below; the
  // authority follows them; the signed tip is checked at the end, over all of them. Nothing is kept
  // before it verifies.
  AuthorityTrail trail = *trail_;
  std::vector<Record> records;
  std::string link = top.hash;
  for (uint64_t height = top.height + 1; height <= signedTip.height; ++height) {
    auto fetched = source_.block(height);
    if (!fetched) {
      return Error(kUnavailable, "Block " + std::to_string(height) + ": " + fetched.error().message);
    }
    const auto &node = fetched.value();
    if (node.block.index != height || chain_block::calculateBlockHash(node.block) != node.hash ||
        node.block.previousHash != link) {
      return Error(kUnavailable, "Block " + std::to_string(height) + " from the node does not link to block " +
                                     std::to_string(height - 1) + " held here");
    }
    if (auto observed = trail.observe(node.block); !observed) {
      return Error(kUnavailable, "Block " + std::to_string(height) + ": " + observed.error().message);
    }
    for (const auto &record : node.block.records) {
      if (AuthorityTrail::changesAuthority(record)) {
        Record step;
        step.kind = Record::Step;
        step.s1 = utl::binaryPack(Client::AuthorityStep{height, node.block.epoch, record});
        records.push_back(std::move(step));
      }
      for (uint64_t id : watching) {
        if (records_.matchesWalletForIndex(record, id)) {
          Record touch;
          touch.kind = Record::Touch;
          touch.a = id;
          touch.b = height;
          records.push_back(std::move(touch));
        }
      }
    }
    Record facts;
    facts.kind = Record::Block;
    facts.a = height;
    facts.b = node.block.slot;
    facts.c = node.block.epoch;
    facts.s1 = node.hash;
    facts.s2 = node.block.stateRoot;
    facts.d = static_cast<uint64_t>(node.block.finalAt);
    records.push_back(std::move(facts));
    link = node.hash;
  }
  if (link != signedTip.blockHash) {
    return Error(kUnavailable, "The blocks from the node are not the ones the signed tip names");
  }
  // The blocks may have changed the authority (a recovery announced or applied, the admin set
  // replaced): the certificate held stands only if it still would be taken, as a node re-checks it.
  auto stands = heldCertificateStands(trail);
  if (!stands) {
    trail.setSeenCertificate(std::nullopt);
  }
  if (auto verified = trail.verifyTip(signedTip); !verified) {
    return Error(kUnavailable, "Signed tip at block " + std::to_string(signedTip.height) + ": " + verified.error().message);
  }
  std::lock_guard lock(mutex_);
  for (const auto &record : records) {
    if (auto written = append(record); !written) {
      return written;
    }
  }
  if (!stands) {
    if (auto dropped = dropHeldCertificate(stands.error().message); !dropped) {
      return dropped;
    }
  }
  trail_ = std::move(trail);
  lastTip_ = signedTip;
  refreshEquivocation();
  return {};
}

// ── accounts ────────────────────────────────────────────────────────────────

Gateway::Roe<bool> Gateway::proveAccounts() {
  const int64_t nowSeconds = now();
  const bool sweep = nowSeconds - lastSweep_ >= options_.sweepSeconds;
  std::vector<uint64_t> due;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[id, account] : accounts_) {
      if (sweep || !account.provenUpTo || !account.touches.empty()) {
        due.push_back(id);
      }
    }
  }
  if (sweep) {
    lastSweep_ = nowSeconds;
  }
  bool ahead = false;
  // Nodes prove at their tip. Touches are scanned only from when an account is watched, so a proof
  // of an older block than the one verified here could miss a change: take none below it.
  const uint64_t floor = facts_.back().height;
  for (uint64_t id : due) {
    auto proof = source_.accountProof(id);
    if (!proof) {
      log_.warning << "Account " << id << ": " << proof.error().message << " (again next round)";
      continue;
    }
    const uint64_t at = proof.value().blockId;
    if (at < floor) {
      log_.warning << "Account " << id << ": the node's proof is of block " << at << ", below the verified tip "
                   << floor << " (again next round)";
      continue;
    }
    const Facts *facts = factsAt(at);
    if (!facts) {
      ahead = ahead || at > facts_.back().height;
      continue;
    }
    Ledger::Block header;
    header.index = at;
    header.stateRoot = facts->stateRoot;
    auto proven = verify::checkAccountProof(header, proof.value(), id);
    if (!proven) {
      log_.warning << "Account " << id << ": the node's proof does not check: " << proven.error().message;
      continue;
    }
    std::lock_guard lock(mutex_);
    auto it = accounts_.find(id);
    if (it == accounts_.end() || (it->second.provenUpTo && at < *it->second.provenUpTo)) {
      continue; // unwatched meanwhile, or an older proof than the one held
    }
    recordProof(id, proven.value().wallet ? &*proven.value().wallet : nullptr, at);
  }
  return ahead;
}

void Gateway::recordProof(uint64_t accountId, const Client::Wallet *wallet, uint64_t provenAt) {
  Account &account = accounts_.at(accountId);
  Entry value{true, wallet != nullptr, wallet ? wallet->mBalances : std::map<uint64_t, int64_t>{}};
  std::vector<std::pair<uint64_t, Entry>> written;
  // A proof shows the account after `provenAt`. With blocks naming it since the last proof, the value
  // holds from the last of them; between the first and the last of them, it is not known.
  std::vector<uint64_t> named(account.touches.begin(), account.touches.upper_bound(provenAt));
  if (!account.provenUpTo) {
    written.emplace_back(provenAt, value);
  } else if (!named.empty()) {
    if (named.front() < named.back()) {
      written.emplace_back(named.front(), Entry{false, false, {}});
    }
    written.emplace_back(named.back(), value);
  } else {
    const Entry *last = account.entries.empty() ? nullptr : &account.entries.rbegin()->second;
    if (!last || !(*last == value)) {
      // Changed with no record seen naming it: somewhere since the last proof.
      const uint64_t from = *account.provenUpTo + 1;
      if (from < provenAt) {
        written.emplace_back(from, Entry{false, false, {}});
      }
      written.emplace_back(provenAt, value);
      Event event;
      event.kind = Event::Unexplained;
      event.accountId = accountId;
      event.height = provenAt;
      event.droppedUpTo = std::min(from, provenAt);
      emit(event);
    }
  }
  for (const auto &[height, entry] : written) {
    Record record;
    record.kind = Record::Balance;
    record.a = accountId;
    record.b = height;
    record.c = entry.known ? 1 : 0;
    record.d = entry.present ? 1 : 0;
    record.balances = entry.balances;
    if (auto ok = append(record); !ok) {
      log_.error << ok.error().message;
      return;
    }
  }
  Record proven;
  proven.kind = Record::Proven;
  proven.a = accountId;
  proven.b = provenAt;
  if (auto ok = append(proven); !ok) {
    log_.error << ok.error().message;
  }
}

Gateway::Roe<void> Gateway::watch(uint64_t accountId) {
  if (accountId >= AccountIds::ID_LIMIT) {
    return Error(kBadRequest, "Not an account id");
  }
  std::lock_guard lock(mutex_);
  if (accounts_.count(accountId)) {
    return {};
  }
  Record record;
  record.kind = Record::Watch;
  record.a = accountId;
  return append(record);
}

Gateway::Roe<void> Gateway::unwatch(uint64_t accountId) {
  std::lock_guard lock(mutex_);
  if (!accounts_.count(accountId)) {
    return Error(kNotFound, "Account " + std::to_string(accountId) + " is not watched");
  }
  Record record;
  record.kind = Record::Unwatch;
  record.a = accountId;
  return append(record);
}

std::vector<uint64_t> Gateway::watched() const {
  std::lock_guard lock(mutex_);
  std::vector<uint64_t> ids;
  for (const auto &[id, account] : accounts_) {
    ids.push_back(id);
  }
  return ids;
}

// ── finality ────────────────────────────────────────────────────────────────

void Gateway::rebuildFinalityHistory() {
  finalityHistory_.clear();
  if (!genesis_) {
    return;
  }
  finalityHistory_.emplace_back(0, config_.finalitySeconds);
  // A config update applies from the epoch after its block's (WIRE_SCHEMA § Config updates).
  for (const auto &step : steps_) {
    if (auto seconds = finalityOfConfigUpdate(step.record); seconds && *seconds != finalityHistory_.back().second) {
      finalityHistory_.emplace_back(step.epoch + 1, *seconds);
    }
  }
}

uint64_t Gateway::finalitySecondsAt(uint64_t epoch) const {
  uint64_t seconds = config_.finalitySeconds;
  for (const auto &[from, bound] : finalityHistory_) {
    if (from > epoch) {
      break;
    }
    seconds = bound;
  }
  return seconds;
}

void Gateway::updateFinalized() {
  const int64_t nowSeconds = now();
  std::lock_guard lock(mutex_);
  if (facts_.empty() || !genesis_) {
    return;
  }
  // Each verified header says when it and every block below it are final (FN007): a running maximum,
  // so the final blocks are a prefix. Nothing else decides it.
  uint64_t height = finalized_ ? *finalized_ + 1 : facts_.front().height;
  while (const Facts *facts = factsAt(height)) {
    if (facts->finalAt > nowSeconds) {
      break;
    }
    finalized_ = height++;
  }
}

// ── views ───────────────────────────────────────────────────────────────────

void Gateway::setClockOverride(std::optional<int64_t> fixed) {
  std::lock_guard lock(mutex_);
  clockOverride_ = fixed;
}

int64_t Gateway::now() const { return clockOverride_ ? *clockOverride_ : static_cast<int64_t>(std::time(nullptr)); }

int64_t Gateway::slotTime(uint64_t slot) const {
  return config_.genesisTime + static_cast<int64_t>(slot * config_.slotDuration);
}

const Gateway::Facts *Gateway::factsAt(uint64_t height) const {
  if (facts_.empty() || height < facts_.front().height || height > facts_.back().height) {
    return nullptr;
  }
  return &facts_[height - facts_.front().height];
}

BlockView Gateway::view(const Facts &facts) const {
  BlockView out;
  out.height = facts.height;
  out.hash = facts.hash;
  out.slot = facts.slot;
  out.slotTime = slotTime(facts.slot);
  out.finalityTime = facts.finalAt;
  out.final = finalized_ && facts.height <= *finalized_;
  return out;
}

std::string Gateway::haltedReason() const {
  if (forkedAt_) {
    return "A certificate vouches for another block at height " + std::to_string(*forkedAt_) +
           " than the one verified here: stopped (start again from a new data directory once the operators decide)";
  }
  if (equivocation_) {
    return "Beacon generation " + std::to_string(equivocation_->generation()) + " signed two blocks at height " +
           std::to_string(equivocation_->height()) + ": nothing above it is followed until a later generation is certified";
  }
  return {};
}

Gateway::Roe<TipView> Gateway::tip() const {
  std::lock_guard lock(mutex_);
  if (facts_.empty() || !genesis_ || !trail_) {
    return Error(kUnavailable, "Not following the chain yet");
  }
  TipView out;
  out.block = view(facts_.back());
  out.generation = trail_->beacon().generation;
  out.finalizedHeight = finalized_;
  out.finalitySeconds = finalitySecondsAt(facts_.back().epoch);
  out.halted = haltedReason();
  return out;
}

Gateway::Roe<BlockView> Gateway::blockAt(uint64_t height) const {
  std::lock_guard lock(mutex_);
  if (!genesis_) {
    return Error(kUnavailable, "Not following the chain yet");
  }
  const Facts *facts = factsAt(height);
  if (!facts) {
    if (!facts_.empty() && height > facts_.back().height) {
      return Error(kConflict, "Block " + std::to_string(height) + " is not verified yet");
    }
    return Error(kNotFound, "Block " + std::to_string(height) + " is below the blocks this gateway followed");
  }
  return view(*facts);
}

Gateway::Roe<uint64_t> Gateway::resolve(At at) const {
  if (facts_.empty() || !genesis_) {
    return Error(kUnavailable, "Not following the chain yet");
  }
  switch (at.kind) {
  case At::Final:
    if (!finalized_) {
      return Error(kConflict, "No block is final yet");
    }
    return *finalized_;
  case At::Tip:
    return facts_.back().height;
  case At::Height: {
    const auto height = static_cast<uint64_t>(at.value);
    if (at.value < 0 || height < facts_.front().height) {
      return Error(kNotFound, "Block " + std::to_string(at.value) + " is below the blocks this gateway followed");
    }
    if (height > facts_.back().height) {
      return Error(kConflict, "Block " + std::to_string(height) + " is not verified yet");
    }
    return height;
  }
  case At::Time: {
    // The last block whose slot started at or before the time.
    auto it = std::upper_bound(facts_.begin(), facts_.end(), at.value,
                               [this](int64_t when, const Facts &facts) { return when < slotTime(facts.slot); });
    if (it == facts_.begin()) {
      return Error(kNotFound, "No block this gateway followed started by then");
    }
    return std::prev(it)->height;
  }
  }
  return Error(kBadRequest, "Unknown height");
}

Gateway::Roe<BalancesView> Gateway::balances(uint64_t accountId, At at) const {
  std::lock_guard lock(mutex_);
  auto it = accounts_.find(accountId);
  if (it == accounts_.end()) {
    return Error(kNotFound, "Account " + std::to_string(accountId) + " is not watched");
  }
  auto height = resolve(at);
  if (!height) {
    return height.error();
  }
  const Account &account = it->second;
  const uint64_t h = height.value();
  if (!account.provenUpTo) {
    return Error(kConflict, "Account " + std::to_string(accountId) + " is not proven yet");
  }
  if (h > *account.provenUpTo && !account.touches.empty() && *account.touches.begin() <= h) {
    return Error(kConflict, "Account " + std::to_string(accountId) + " changed by block " +
                                std::to_string(*account.touches.begin()) + ", not proven yet");
  }
  auto entry = account.entries.upper_bound(h);
  if (entry == account.entries.begin()) {
    return Error(kConflict, "Account " + std::to_string(accountId) + " was not watched at block " + std::to_string(h));
  }
  --entry;
  if (!entry->second.known) {
    return Error(kConflict, "Account " + std::to_string(accountId) + "'s balances at block " + std::to_string(h) +
                                " are not known (it changed more than once between proofs)");
  }
  BalancesView out;
  out.accountId = accountId;
  out.height = h;
  out.present = entry->second.present;
  out.balances = entry->second.balances;
  return out;
}

std::vector<Event> Gateway::events(uint64_t afterSeq, size_t limit) const {
  std::lock_guard lock(mutex_);
  std::vector<Event> out;
  auto it = std::upper_bound(events_.begin(), events_.end(), afterSeq,
                             [](uint64_t seq, const Event &event) { return seq < event.seq; });
  for (; it != events_.end() && out.size() < limit; ++it) {
    out.push_back(*it);
  }
  return out;
}

} // namespace pp::gateway
