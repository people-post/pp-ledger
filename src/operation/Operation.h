#pragma once
// Operation files (docs/contracts/OPERATION_FILE.md): a transaction that needs several signatures,
// carried between its signers as a file — what it is, who must sign it, the signatures so far. The
// admin apps and pp-client read, build, sign, merge and submit them. No network code: an offline
// signer links this library alone.

#include "../ledger/Account.h"
#include "../ledger/Ledger.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp::operation {

inline constexpr const char *kFormat = "pp-ledger-operation";
inline constexpr uint32_t kVersion = 1;

/** What an operation does, as its record says (never as the file claims). */
enum class Kind { Transfer, Mint, NewAccount, ConfigUpdate, BeaconCertificate, Recovery, Genesis, Other };

std::string kindName(Kind kind);
std::optional<Kind> kindFromName(const std::string &name);
/** The kind of `record`, from its type and fields: a transfer from genesis to the reserve mints. */
Kind kindOf(const Ledger::Record &record);
/**
 * The account whose keys sign `record`, as the chain resolves it: the payer of a transfer or new
 * account, the genesis (admin) account of a config update, beacon certificate or genesis. None for a
 * recovery (the recovery key set signs it) or a kind the operation file does not know (Other).
 */
std::optional<uint64_t> signerAccountOf(const Ledger::Record &record);

/** Who must sign: `minSignatures` distinct keys of `publicKeys` (an account's, or a key set's). */
struct SignerSet {
  /** The account that pays or acts (none for a key set: the recovery set). */
  std::optional<uint64_t> account;
  uint32_t minSignatures{1};
  /** Raw ML-DSA-65 public keys. */
  std::vector<std::string> publicKeys;

  bool operator==(const SignerSet &other) const = default;
};

struct Operation {
  std::string networkId;
  /** Raw 32 bytes of the chain's block 0 hash; empty when the creator did not know it. */
  std::string genesisHash;
  /** Set from the record (kindOf) when built or read. */
  Kind kind{Kind::Other};
  /** The record without its signatures: type and data, what is signed. */
  Ledger::Record record;
  SignerSet signers;
  /** Raw signatures of `record` collected, in any order (each by a distinct key of `signers`). */
  std::vector<std::string> signatures;
  /**
   * A genesis only: block 0's records after the genesis record (system accounts, genesis miners),
   * signed by the same keys; each carries its own signatures in `signatures`. A key has signed the
   * operation once it has signed every record.
   */
  std::vector<Ledger::Record> moreRecords;
  /** The creator's free text: shown as theirs, never as what the operation does. */
  std::string notes;
  int64_t createdAt{0};
};

/** What is signed: the record's signing message for the operation's network. */
std::string signingMessage(const Operation &op);

/** The JSON file. */
std::string toJson(const Operation &op);
/**
 * Read a file's JSON: format and version known, fields well formed, `kind` as the record says.
 * Signatures are not checked here (check()).
 */
pp::Roe<Operation> fromJson(const std::string &json);
pp::Roe<Operation> load(const std::string &path);
/** Write `op` to `path`; replaces an existing file atomically when `overwrite`, else refuses one. */
pp::Roe<void> save(const Operation &op, const std::string &path, bool overwrite = false);

/** A public key as people compare it: 16 hex digits of its SHA-256, in groups of four. */
std::string fingerprint(const std::string &publicKey);

struct SignatureCheck {
  /** The keys that signed (a valid signature of every record), in `signers.publicKeys` order. */
  std::vector<std::string> signedBy;
  /** Signatures (of any record) that verify under no listed key, or repeat a key already counted. */
  size_t invalid{0};
  uint32_t needed{0};
  bool complete() const { return signedBy.size() >= needed; }
};

SignatureCheck check(const Operation &op);

/**
 * Sign `op` (every record) with `privateKey`: the key must be one of the signers', and not have
 * signed already. Returns the public key that signed.
 */
pp::Roe<std::string> sign(Operation &op, const std::string &privateKey);

/**
 * Copies of one operation signed apart, as one: everything but the signatures must be the same;
 * the valid signatures of all, each key once.
 */
pp::Roe<Operation> merge(const std::vector<Operation> &copies);

/** The record as a node takes it: with the valid signatures. Refused short of the quorum. */
pp::Roe<Ledger::Record> signedRecord(const Operation &op);
/** Every record (`record`, then `moreRecords`) with its valid signatures. Refused short of the quorum. */
pp::Roe<std::vector<Ledger::Record>> signedRecords(const Operation &op);

/** What an operation does, for people: recomputed from the record, every time. */
struct Summary {
  std::string title;
  std::vector<std::pair<std::string, std::string>> lines;
  /** The record's validity window (Unix seconds); 0 when it has none. */
  int64_t validFrom{0};
  int64_t validUntil{0};
};

pp::Roe<Summary> summarize(const Operation &op);

/** An amount as people read it: digits grouped by thousands ("1,000,000"). */
std::string groupDigits(uint64_t value);

// ---- Builders ----

struct Window {
  int64_t from{0};
  int64_t until{0};
};
/** A window opening a minute before `now` and lasting `validForSeconds` (the chain caps it). */
Window windowFrom(int64_t now, int64_t validForSeconds);

struct Common {
  std::string networkId;
  std::string genesisHash;
  SignerSet signers;
  Window window;
  /** 0: a random one. */
  uint64_t idempotentId{0};
  std::string notes;
  int64_t createdAt{0};
};

/** A transfer of `amount` of `tokenId` (0: the native token) from the signers' account to `to`. */
pp::Roe<Operation> transfer(const Common &common, uint64_t to, uint64_t amount, uint64_t fee, uint64_t tokenId = 0);
/** New native tokens: a transfer from the genesis account to the reserve (the only way to mint). */
pp::Roe<Operation> mint(const Common &common, uint64_t amount, uint64_t fee);
/** A new account `id` funded with `amount` by the signers' account (the registrar for issued ids). */
pp::Roe<Operation> newAccount(const Common &common, uint64_t id, const UserAccount &account, uint64_t amount,
                              uint64_t fee);
/** A record built elsewhere (pp-client mk-beacon-cert, mk-recovery, mk-tx), its signatures dropped. */
pp::Roe<Operation> wrap(const Common &common, const Ledger::Record &record);
/**
 * Block 0 to sign offline (chain_genesis::records): the genesis record first. Its network and
 * signers (the genesis account's keys) come from the genesis record; no genesis hash (there is no
 * block 0 yet).
 */
pp::Roe<Operation> genesis(const std::vector<Ledger::Record> &records, const std::string &notes, int64_t createdAt);

} // namespace pp::operation
