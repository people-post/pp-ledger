#include "AmpLedgerTransport.h"
#include "Client.h"
#include <algorithm>
#include <variant>
#include <thread>
#include <random>
#include "common/Logger.h"
#include "lib/common/BinaryPack.hpp"
#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"

#include <sstream>

namespace pp {


std::ostream& operator<<(std::ostream& os, const Client::Request& req) {
  os << "Request{version=" << req.version << ", type=" << req.type << ", payload=" << req.payload.size() << " bytes}";
  return os;
}

std::string Client::SignedTip::signingMessage() const {
  return std::string("pp-ledger/signed-tip/v1") + utl::binaryPack(networkId) + utl::binaryPack(generation) +
         utl::binaryPack(height) + utl::binaryPack(blockHash);
}

bool Client::SignedTip::verify(const std::string &publicKey) const {
  return utl::mlDsaVerify(publicKey, signingMessage(), signature);
}

std::string Client::SignedTip::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Client::SignedTip::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  return !ar.failed();
}

std::string Client::RewindReport::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Client::RewindReport::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  return !ar.failed();
}

std::optional<Client::Equivocation> Client::Equivocation::of(const SignedTip &a, const SignedTip &b) {
  if (a.networkId != b.networkId || a.generation != b.generation || a.height != b.height ||
      a.blockHash == b.blockHash) {
    return std::nullopt;
  }
  Equivocation evidence;
  evidence.first = a.blockHash < b.blockHash ? a : b;
  evidence.second = a.blockHash < b.blockHash ? b : a;
  return evidence;
}

bool Client::Equivocation::verify(const std::string &publicKey) const {
  auto shaped = of(first, second);
  return shaped && *shaped == *this && first.verify(publicKey) && second.verify(publicKey);
}

std::string Client::Equivocation::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool Client::Equivocation::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  return !ar.failed();
}

std::string Client::MinerInfo::signingMessage(const std::string &networkId) const {
  return std::string("pp-ledger/miner-register/v2") + utl::binaryPack(networkId) + utl::binaryPack(id) +
         utl::binaryPack(issuedAt);
}

pp::common::Meta Client::MinerInfo::ltsToMeta() const {
  pp::common::Meta m;
  m.setUIntForJson("id", id);
  m.set("tLastMessage", tLastMessage);
  m.set("issuedAt", issuedAt);
  m.set("signatures", utl::binaryPack(signatures));
  return m;
}

Client::Roe<bool> Client::MinerInfo::ltsFromMeta(const pp::common::Meta &meta) {
  id = meta.getNonNegInt("id").value_or(0);
  tLastMessage = meta.getOrDefault("tLastMessage", int64_t{0});
  issuedAt = meta.getOrDefault("issuedAt", int64_t{0});
  signatures.clear();
  if (auto packed = meta.getOrDefault("signatures", std::string{}); !packed.empty()) {
    auto unpacked = utl::binaryUnpack<std::vector<std::string>>(packed);
    if (!unpacked) {
      return Error(E_PARSE_ERROR, "Invalid miner signatures: " + unpacked.error().message);
    }
    signatures = std::move(unpacked.value());
  }
  return true;
}

pp::common::Meta Client::MinerStatus::ltsToMeta() const {
  pp::common::Meta m;
  m.setUIntForJson("minerId", minerId);
  m.setUIntForJson("stake", stake);
  m.setUIntForJson("nextBlockId", nextBlockId);
  m.setUIntForJson("currentSlot", currentSlot);
  m.setUIntForJson("currentEpoch", currentEpoch);
  m.setUIntForJson("pendingTransactions", pendingTransactions);
  m.setUIntForJson("nStakeholders", nStakeholders);
  m.set("isSlotLeader", isSlotLeader);
  m.set("networkId", networkId);
  return m;
}

Client::Roe<bool> Client::MinerStatus::ltsFromMeta(const pp::common::Meta &meta) {
  minerId = meta.getNonNegInt("minerId").value_or(0);
  stake = meta.getNonNegInt("stake").value_or(0);
  nextBlockId = meta.getNonNegInt("nextBlockId").value_or(0);
  currentSlot = meta.getNonNegInt("currentSlot").value_or(0);
  currentEpoch = meta.getNonNegInt("currentEpoch").value_or(0);
  pendingTransactions = meta.getNonNegInt("pendingTransactions").value_or(0);
  nStakeholders = meta.getNonNegInt("nStakeholders").value_or(0);
  isSlotLeader = meta.getOrDefault("isSlotLeader", false);
  networkId = meta.getOrDefault("networkId", std::string{});
  return true;
}

pp::common::Meta Client::BeaconState::ltsToMeta() const {
  pp::common::Meta m;
  m.set("currentTimestamp", currentTimestamp);
  m.setUIntForJson("checkpointId", checkpointId);
  m.setUIntForJson("nextBlockId", nextBlockId);
  m.setUIntForJson("currentSlot", currentSlot);
  m.setUIntForJson("currentEpoch", currentEpoch);
  m.setUIntForJson("nStakeholders", nStakeholders);
  if (!networkId.empty()) {
    m.set("networkId", networkId);
  }
  if (!headHash.empty()) {
    m.set("headHash", headHash);
  }
  if (registryVersion != 0) {
    m.setUIntForJson("registryVersion", registryVersion);
  }
  if (signedTip) {
    m.set("signedTip", signedTip->ltsToString());
  }
  if (certificate) {
    m.set("certificate", utl::binaryPack(*certificate));
  }
  if (equivocation) {
    m.set("equivocation", equivocation->ltsToString());
  }
  if (rewound) {
    m.set("rewound", rewound->ltsToString());
  }
  if (recovery) {
    m.set("recovery", utl::binaryPack(*recovery));
  }
  return m;
}

Client::Roe<bool> Client::BeaconState::ltsFromMeta(const pp::common::Meta &meta) {
  currentTimestamp = meta.getOrDefault("currentTimestamp", int64_t{0});
  checkpointId = meta.getNonNegInt("checkpointId").value_or(0);
  nextBlockId = meta.getNonNegInt("nextBlockId").value_or(0);
  currentSlot = meta.getNonNegInt("currentSlot").value_or(0);
  currentEpoch = meta.getNonNegInt("currentEpoch").value_or(0);
  nStakeholders = meta.getNonNegInt("nStakeholders").value_or(0);
  networkId = meta.getOrDefault("networkId", std::string{});
  headHash = meta.getOrDefault("headHash", std::string{});
  registryVersion = meta.getNonNegInt("registryVersion").value_or(0);
  signedTip.reset();
  if (auto packed = meta.getOrDefault("signedTip", std::string{}); !packed.empty()) {
    SignedTip tip;
    if (!tip.ltsFromString(packed)) {
      return Error(E_INVALID_RESPONSE, "Invalid signed tip in beacon state");
    }
    signedTip = std::move(tip);
  }
  certificate.reset();
  if (auto packed = meta.getOrDefault("certificate", std::string{}); !packed.empty()) {
    auto record = utl::binaryUnpack<Ledger::Record>(packed);
    if (!record || record.value().type != Ledger::T_BEACON_CERT) {
      return Error(E_INVALID_RESPONSE, "Invalid beacon certificate in beacon state");
    }
    certificate = std::move(record.value());
  }
  equivocation.reset();
  if (auto packed = meta.getOrDefault("equivocation", std::string{}); !packed.empty()) {
    Equivocation evidence;
    if (!evidence.ltsFromString(packed)) {
      return Error(E_INVALID_RESPONSE, "Invalid equivocation evidence in beacon state");
    }
    equivocation = std::move(evidence);
  }
  recovery.reset();
  if (auto packed = meta.getOrDefault("recovery", std::string{}); !packed.empty()) {
    auto status = utl::binaryUnpack<RecoveryStatus>(packed);
    if (!status) {
      return Error(E_INVALID_RESPONSE, "Invalid recovery status in beacon state");
    }
    recovery = std::move(status.value());
  }
  rewound.reset();
  if (auto packed = meta.getOrDefault("rewound", std::string{}); !packed.empty()) {
    RewindReport report;
    if (!report.ltsFromString(packed)) {
      return Error(E_INVALID_RESPONSE, "Invalid rewind report in beacon state");
    }
    rewound = report;
  }
  return true;
}

std::string Client::requestTypeName(uint32_t type) {
  switch (type) {
  case T_REQ_STATUS:
    return "status";
  case T_REQ_REGISTER:
    return "register";
  case T_REQ_MINER_LIST:
    return "miner_list";
  case T_REQ_CALIBRATION:
    return "calibration";
  case T_REQ_HEALTH:
    return "health";
  case T_REQ_BLOCK_GET:
    return "block_get";
  case T_REQ_BLOCK_ADD:
    return "block_add";
  case T_REQ_BLOCK_WAIT:
    return "block_wait";
  case T_REQ_AUTHORITY_PROOF:
    return "authority_proof";
  case T_REQ_EQUIVOCATION_ADD:
    return "equivocation_add";
  case T_REQ_PRODUCER_STATS:
    return "producer_stats";
  case T_REQ_ACCOUNT_PROOF:
    return "account_proof";
  case T_REQ_ACCOUNT_GET:
    return "account_get";
  case T_REQ_CONFIG_GET:
    return "config_get";
  case T_REQ_DOMAIN_GET:
    return "domain_get";
  case T_REQ_NAME_GET:
    return "name_get";
  case T_REQ_NAME_GET_BY_WALLET:
    return "name_get_by_wallet";
  case T_REQ_TX_GET_BY_WALLET:
    return "tx_get_by_wallet";
  case T_REQ_TX_ADD:
    return "tx_add";
  case T_REQ_TX_GET_BY_INDEX:
    return "tx_get_by_index";
  case T_REQ_TX_PULL:
    return "tx_pull";
  default:
    return "type_" + std::to_string(type);
  }
}

pp::common::Meta Client::NodeHealth::ltsToMeta() const {
  pp::common::Meta m;
  m.set("role", role);
  m.set("peerId", peerId);
  m.set("networkId", networkId);
  m.set("ready", ready);
  if (!notReady.empty()) {
    m.set("notReady", notReady);
  }
  m.setUIntForJson("nextBlockId", nextBlockId);
  if (upstreamNextBlockId) {
    m.setUIntForJson("upstreamNextBlockId", *upstreamNextBlockId);
  }
  if (lastSyncAgeSeconds) {
    m.set("lastSyncAgeSeconds", *lastSyncAgeSeconds);
  }
  if (signedTipHeight) {
    m.setUIntForJson("signedTipHeight", *signedTipHeight);
  }
  m.setUIntForJson("generation", generation);
  m.set("halted", halted);
  m.set("recoveryPending", recoveryPending);
  m.set("standingDown", standingDown);
  m.setUIntForJson("requestQueueDepth", requestQueueDepth);
  m.set("reportAgeMs", reportAgeMs);
  std::vector<pp::common::Meta::Value> counts;
  counts.reserve(requests.size());
  for (const auto &count : requests) {
    auto c = std::make_shared<pp::common::Meta>();
    c->setUIntForJson("type", count.type);
    c->setUIntForJson("served", count.served);
    c->setUIntForJson("waitMs", count.waitMs);
    c->setUIntForJson("serviceMs", count.serviceMs);
    counts.push_back(std::move(c));
  }
  m.set("requests", pp::common::Meta::array(std::move(counts)));
  m.setUIntForJson("requestsBusy", requestsBusy);
  m.setUIntForJson("requestsRefused", requestsRefused);
  m.setUIntForJson("requestsExpired", requestsExpired);
  m.setUIntForJson("loopTurnsSlow", loopTurnsSlow);
  m.setUIntForJson("loopTurnMaxMs", loopTurnMaxMs);
  const auto setOptional = [&m](const char *key, const std::optional<uint64_t> &value) {
    if (value) {
      m.setUIntForJson(key, *value);
    }
  };
  setOptional("rssBytes", rssBytes);
  setOptional("udpReceiveBufferBytes", udpReceiveBufferBytes);
  setOptional("udpReceiveDrops", udpReceiveDrops);
  m.setUIntForJson("minerPeers", minerPeers);
  setOptional("links", links);
  setOptional("blocksSealed", blocksSealed);
  setOptional("blocksCommitted", blocksCommitted);
  setOptional("blocksAbandoned", blocksAbandoned);
  if (lastSealDelayMs) {
    m.set("lastSealDelayMs", *lastSealDelayMs);
  }
  setOptional("txPoolRecords", txPoolRecords);
  setOptional("txPoolBytes", txPoolBytes);
  if (txPoolOldestAgeSeconds) {
    m.set("txPoolOldestAgeSeconds", *txPoolOldestAgeSeconds);
  }
  setOptional("txPoolAdded", txPoolAdded);
  setOptional("txPoolDuplicates", txPoolDuplicates);
  setOptional("txPoolFull", txPoolFull);
  setOptional("txPoolExpired", txPoolExpired);
  setOptional("txPoolIncluded", txPoolIncluded);
  setOptional("txPoolSenderFull", txPoolSenderFull);
  setOptional("txPoolDropped", txPoolDropped);
  return m;
}

Client::Roe<bool> Client::NodeHealth::ltsFromMeta(const pp::common::Meta &meta) {
  role = meta.getOrDefault("role", std::string{});
  peerId = meta.getOrDefault("peerId", std::string{});
  networkId = meta.getOrDefault("networkId", std::string{});
  ready = meta.getOrDefault("ready", false);
  notReady = meta.getOrDefault("notReady", std::string{});
  nextBlockId = meta.getNonNegInt("nextBlockId").value_or(0);
  upstreamNextBlockId = meta.getNonNegInt("upstreamNextBlockId");
  lastSyncAgeSeconds.reset();
  if (meta.contains("lastSyncAgeSeconds")) {
    lastSyncAgeSeconds = meta.getOrDefault("lastSyncAgeSeconds", int64_t{0});
  }
  signedTipHeight = meta.getNonNegInt("signedTipHeight");
  generation = meta.getNonNegInt("generation").value_or(0);
  halted = meta.getOrDefault("halted", false);
  recoveryPending = meta.getOrDefault("recoveryPending", false);
  standingDown = meta.getOrDefault("standingDown", false);
  requestQueueDepth = meta.getNonNegInt("requestQueueDepth").value_or(0);
  reportAgeMs = meta.getOrDefault("reportAgeMs", int64_t{-1});
  requests.clear();
  if (const auto *counts = meta.getArray("requests")) {
    for (const auto &value : counts->elements) {
      const auto *c = pp::common::asObject(value);
      if (!c) {
        continue;
      }
      RequestCount count;
      count.type = static_cast<uint32_t>(c->getNonNegInt("type").value_or(0));
      count.served = c->getNonNegInt("served").value_or(0);
      count.waitMs = c->getNonNegInt("waitMs").value_or(0);
      count.serviceMs = c->getNonNegInt("serviceMs").value_or(0);
      requests.push_back(count);
    }
  }
  requestsBusy = meta.getNonNegInt("requestsBusy").value_or(0);
  requestsRefused = meta.getNonNegInt("requestsRefused").value_or(0);
  requestsExpired = meta.getNonNegInt("requestsExpired").value_or(0);
  loopTurnsSlow = meta.getNonNegInt("loopTurnsSlow").value_or(0);
  loopTurnMaxMs = meta.getNonNegInt("loopTurnMaxMs").value_or(0);
  rssBytes = meta.getNonNegInt("rssBytes");
  udpReceiveBufferBytes = meta.getNonNegInt("udpReceiveBufferBytes");
  udpReceiveDrops = meta.getNonNegInt("udpReceiveDrops");
  minerPeers = meta.getNonNegInt("minerPeers").value_or(0);
  links = meta.getNonNegInt("links");
  blocksSealed = meta.getNonNegInt("blocksSealed");
  blocksCommitted = meta.getNonNegInt("blocksCommitted");
  blocksAbandoned = meta.getNonNegInt("blocksAbandoned");
  lastSealDelayMs.reset();
  if (meta.contains("lastSealDelayMs")) {
    lastSealDelayMs = meta.getOrDefault("lastSealDelayMs", int64_t{0});
  }
  txPoolRecords = meta.getNonNegInt("txPoolRecords");
  txPoolBytes = meta.getNonNegInt("txPoolBytes");
  txPoolOldestAgeSeconds.reset();
  if (meta.contains("txPoolOldestAgeSeconds")) {
    txPoolOldestAgeSeconds = meta.getOrDefault("txPoolOldestAgeSeconds", int64_t{0});
  }
  txPoolAdded = meta.getNonNegInt("txPoolAdded");
  txPoolDuplicates = meta.getNonNegInt("txPoolDuplicates");
  txPoolFull = meta.getNonNegInt("txPoolFull");
  txPoolExpired = meta.getNonNegInt("txPoolExpired");
  txPoolIncluded = meta.getNonNegInt("txPoolIncluded");
  txPoolSenderFull = meta.getNonNegInt("txPoolSenderFull");
  txPoolDropped = meta.getNonNegInt("txPoolDropped");
  if (role.empty()) {
    return Error(E_INVALID_RESPONSE, "Health without a role");
  }
  return true;
}

pp::common::Meta Client::TxGetByWalletResponse::ltsToMeta() const {
  std::vector<pp::common::Meta::Value> txs;
  txs.reserve(transactions.size());
  for (const auto &tx : transactions) {
    txs.push_back(std::make_shared<pp::common::Meta>(tx.ltsToMeta()));
  }
  pp::common::Meta j;
  j.set("transactions", pp::common::Meta::array(std::move(txs)));
  j.setUIntForJson("nextBlockId", nextBlockId);
  return j;
}

pp::common::Meta Client::CalibrationResponse::ltsToMeta() const {
  pp::common::Meta j;
  j.set("msTimestamp", msTimestamp);
  j.setUIntForJson("nextBlockId", nextBlockId);
  return j;
}

Client::Client() = default;

Client::~Client() = default;

void Client::attachAmpTransport(network::LedgerAmpRuntime& runtime, std::string default_peer_key) {
  amp_default_peer_key_ = std::move(default_peer_key);
  transport_ = std::make_unique<AmpLedgerTransport>(runtime, amp_default_peer_key_);
  if (auto* amp = dynamic_cast<AmpLedgerTransport*>(transport_.get())) {
    amp->redirectLogger(log().getFullName() + ".AmpLedgerTransport");
  }
}

Client::Roe<void> Client::setAmpPeer(const std::string& peer_key, const std::string& multiaddr) {
  auto* amp = dynamic_cast<AmpLedgerTransport*>(transport_.get());
  if (!amp) {
    return Error(E_NOT_CONNECTED, "AMP transport not configured");
  }
  if (!amp->registerEndpoint(peer_key, multiaddr)) {
    return Error(E_NOT_CONNECTED, "Failed to register peer endpoint: " + multiaddr);
  }
  amp->setPeerKey(peer_key);
  return {};
}

void Client::setTransport(std::unique_ptr<ILedgerTransport> transport) {
  transport_ = std::move(transport);
}

std::string Client::getErrorMessage(uint16_t errorCode) {
  switch (errorCode) {
  case E_NOT_CONNECTED:
    return "Not connected to server";
  case E_INVALID_RESPONSE:
    return "Invalid response from server";
  case E_SERVER_ERROR:
    return "Server error";
  case E_PARSE_ERROR:
    return "Failed to parse response";
  case E_REQUEST_FAILED:
    return "Request failed";
  default:
    return "Unknown error";
  }
}

std::string Client::packRequest(uint32_t type, const std::string &payload, std::chrono::milliseconds timeout) {
  Request req;
  req.version = Request::VERSION;
  req.type = type;
  req.payload = payload;
  req.deadlineMs = static_cast<uint32_t>(std::clamp<int64_t>(timeout.count(), 0, UINT32_MAX));
  log().debug << "Sending binary request: " << req;
  return utl::binaryPack(req);
}

Client::Roe<std::string> Client::parseResponse(const ILedgerTransport::Roe<std::string> &result) {
  if (!result.isOk()) {
    if (isOverloadCode(result.error().code)) { // the way to the peer is full (InFlightLimit): busy, not failed
      return Error(result.error().code, result.error().message);
    }
    return Error(E_REQUEST_FAILED,
                 getErrorMessage(E_REQUEST_FAILED) + ": " + result.error().message);
  }
  auto respResult = utl::binaryUnpack<Response>(result.value());
  if (!respResult) {
    return Error(E_INVALID_RESPONSE, respResult.error().message);
  }
  const Response &resp = respResult.value();
  if (resp.isError()) {
    Error error(isOverloadCode(resp.errorCode) ? resp.errorCode : E_SERVER_ERROR, resp.payload);
    error.retryAfterMs = resp.retryAfterMs;
    return error;
  }
  return resp.payload;
}

Client::Roe<std::string> Client::sendRequest(uint32_t type, const std::string &payload,
                                             std::chrono::milliseconds timeout) {
  if (!transport_) {
    return Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED));
  }
  return parseResponse(transport_->roundTrip(packRequest(type, payload, timeout), timeout));
}

void Client::sendRequestAsync(uint32_t type, const std::string &payload,
                              std::chrono::milliseconds timeout, Done<std::string> done) {
  if (!transport_) {
    done(Error(E_NOT_CONNECTED, getErrorMessage(E_NOT_CONNECTED)));
    return;
  }
  transport_->roundTripAsync(packRequest(type, payload, timeout), timeout,
                             [done = std::move(done)](ILedgerTransport::Roe<std::string> result) {
                               done(parseResponse(result));
                             });
}

void Client::requestAsync(uint32_t type, const std::string &payload, Done<std::string> done,
                          std::chrono::milliseconds timeout) {
  const bool carriesData = type == T_REQ_TX_ADD || type == T_REQ_BLOCK_ADD || type == T_REQ_BLOCK_GET ||
                           type == T_REQ_TX_GET_BY_WALLET;
  sendRequestAsync(type, payload, orDefault(timeout, carriesData ? dataTimeout() : fastTimeout()), std::move(done));
}

// BeaconServer API - Block operations (binary T_REQ_BLOCK_GET)

Client::Roe<Ledger::ChainNode> Client::fetchBlock(uint64_t blockId) {
  log().debug << "Requesting block " << blockId;

  std::string payload = utl::binaryPack(blockId);
  auto result = sendRequest(T_REQ_BLOCK_GET, payload, dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return parseBlock(result.value());
}

Client::Roe<Client::AuthorityProof> Client::fetchAuthorityProof(uint64_t upToBlockId) {
  auto result = sendRequest(T_REQ_AUTHORITY_PROOF, utl::binaryPack(upToBlockId), dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  auto proof = utl::binaryUnpack<AuthorityProof>(result.value());
  if (!proof) {
    return Error(E_INVALID_RESPONSE, "Invalid authority proof: " + proof.error().message);
  }
  return proof.value();
}

Client::Roe<Ledger::ChainNode> Client::parseBlock(const std::string &payload) {
  Ledger::ChainNode node;
  if (!node.ltsFromString(payload)) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize block");
  }
  return node;
}

void Client::fetchBlockAsync(uint64_t blockId, Done<Ledger::ChainNode> done) {
  sendRequestAsync(T_REQ_BLOCK_GET, utl::binaryPack(blockId), dataTimeout(),
                   [done = std::move(done)](Roe<std::string> result) {
                     done(result ? parseBlock(result.value())
                                 : Roe<Ledger::ChainNode>(Error(result.error().code, result.error().message)));
                   });
}

Client::Roe<Client::TipReply> Client::parseTipReply(const std::string &payload) {
  auto reply = utl::binaryUnpack<TipReply>(payload);
  if (!reply) {
    return Error(E_INVALID_RESPONSE, "Invalid tip reply: " + reply.error().message);
  }
  return reply.value();
}

void Client::waitForBlockAsync(uint64_t knownNextBlockId, Done<TipReply> done) {
  sendRequestAsync(T_REQ_BLOCK_WAIT, utl::binaryPack(knownNextBlockId), fastTimeout(),
                   [done = std::move(done)](Roe<std::string> result) {
                     done(result ? parseTipReply(result.value())
                                 : Roe<TipReply>(Error(result.error().code, result.error().message)));
                   });
}

Client::Roe<Client::UserAccount> Client::fetchUserAccount(const uint64_t accountId) {
  log().debug << "Requesting user account: " << accountId;

  std::string payload = utl::binaryPack(accountId);
  auto result = sendRequest(T_REQ_ACCOUNT_GET, payload, dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  UserAccount account;
  if (!account.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize user account");
  }
  return account;
}

Client::Roe<GenesisAccountMeta> Client::fetchGenesisMeta() {
  auto result = sendRequest(T_REQ_CONFIG_GET, "", dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  GenesisAccountMeta meta;
  if (!meta.ltsFromString(result.value())) {
    return Error(E_INVALID_RESPONSE, "Failed to deserialize the genesis meta");
  }
  return meta;
}

Client::Roe<Client::BeaconState> Client::parseBeaconState(const std::string &payload) {
  auto metaResult = utl::binaryUnpack<pp::common::Meta>(payload);
  if (!metaResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack beacon state Meta: " + metaResult.error().message);
  }
  BeaconState state;
  auto parseResult = state.ltsFromMeta(metaResult.value());
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }
  return state;
}

Client::Roe<Client::NodeHealth> Client::fetchHealth() {
  auto result = sendRequest(T_REQ_HEALTH, "", fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  auto meta = utl::binaryUnpack<pp::common::Meta>(result.value());
  if (!meta) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack health Meta: " + meta.error().message);
  }
  NodeHealth health;
  if (auto parsed = health.ltsFromMeta(meta.value()); !parsed) {
    return Error(parsed.error().code, parsed.error().message);
  }
  return health;
}

Client::Roe<Client::BeaconState> Client::registerMinerServer(const MinerInfo &minerInfo) {
  log().debug << "Registering miner server: " << minerInfo.id;

  std::string payload = utl::binaryPack(minerInfo.ltsToMeta());
  auto result = sendRequest(T_REQ_REGISTER, payload, fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return parseBeaconState(result.value());
}

void Client::registerMinerServerAsync(const MinerInfo &minerInfo, Done<BeaconState> done,
                                      std::chrono::milliseconds timeout) {
  sendRequestAsync(T_REQ_REGISTER, utl::binaryPack(minerInfo.ltsToMeta()), orDefault(timeout, fastTimeout()),
                   [done = std::move(done)](Roe<std::string> result) {
                     done(result ? parseBeaconState(result.value()) : Roe<BeaconState>(result.error()));
                   });
}

Client::Roe<Client::BeaconState> Client::fetchBeaconState() {
  log().debug << "Requesting beacon state (checkpoint, block)";

  auto result = sendRequest(T_REQ_STATUS, "", fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return parseBeaconState(result.value());
}

std::string Client::withRemainingDeadline(const std::string &body, uint32_t remainingMs) {
  auto request = utl::binaryUnpack<Request>(body);
  if (!request || request.value().version < 2 || request.value().deadlineMs == 0 ||
      request.value().deadlineMs <= remainingMs || utl::binaryPack(request.value()) != body) {
    return body;
  }
  request.value().deadlineMs = remainingMs;
  return utl::binaryPack(request.value());
}

Client::Roe<std::string> Client::fetchNetworkId() {
  auto state = fetchBeaconState();
  if (!state) {
    return Error(state.error().code, "Cannot read the node's network id: " + state.error().message);
  }
  if (state.value().networkId.empty()) {
    return Error(E_INVALID_RESPONSE, "The node did not say which network it is on (no networkId in its STATUS)");
  }
  return state.value().networkId;
}

void Client::fetchBeaconStateAsync(Done<BeaconState> done) {
  sendRequestAsync(T_REQ_STATUS, "", fastTimeout(), [done = std::move(done)](Roe<std::string> result) {
    done(result ? parseBeaconState(result.value())
                : Roe<BeaconState>(Error(result.error().code, result.error().message)));
  });
}

Client::Roe<Client::CalibrationResponse> Client::fetchCalibration() {
  log().debug << "Requesting precise timestamp for calibration";

  auto result = sendRequest(T_REQ_CALIBRATION, "", fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return parseCalibration(result.value());
}

Client::Roe<Client::CalibrationResponse> Client::parseCalibration(const std::string &payload) {
  auto roe = utl::binaryUnpack<CalibrationResponse>(payload);
  if (!roe) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack calibration response: " + roe.error().message);
  }
  return roe.value();
}

void Client::fetchCalibrationAsync(Done<CalibrationResponse> done) {
  sendRequestAsync(T_REQ_CALIBRATION, "", fastTimeout(), [done = std::move(done)](Roe<std::string> result) {
    done(result ? parseCalibration(result.value())
                : Roe<CalibrationResponse>(Error(result.error().code, result.error().message)));
  });
}

Client::Roe<std::vector<Client::MinerInfo>> Client::parseMinerList(const std::string &payload) {
  auto listResult = utl::binaryUnpack<std::vector<pp::common::Meta>>(payload);
  if (!listResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack miner list: " + listResult.error().message);
  }
  std::vector<MinerInfo> miners;
  miners.reserve(listResult.value().size());
  for (const auto &meta : listResult.value()) {
    MinerInfo info;
    auto parseResult = info.ltsFromMeta(meta);
    if (!parseResult) {
      return Error(parseResult.error().code, parseResult.error().message);
    }
    miners.push_back(std::move(info));
  }
  return miners;
}

Client::Roe<std::vector<Client::MinerInfo>> Client::fetchMinerList() {
  log().debug << "Requesting miner list";

  auto result = sendRequest(T_REQ_MINER_LIST, "", fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  return parseMinerList(result.value());
}

void Client::fetchMinerListAsync(Done<std::vector<MinerInfo>> done, std::chrono::milliseconds timeout) {
  sendRequestAsync(T_REQ_MINER_LIST, "", orDefault(timeout, fastTimeout()), [done = std::move(done)](Roe<std::string> result) {
    done(result ? parseMinerList(result.value()) : Roe<std::vector<MinerInfo>>(result.error()));
  });
}

Client::Roe<Client::DomainProof> Client::fetchDomainProof(const std::string &domain) {
  log().debug << "Requesting the proof of domain " << domain;
  auto result = sendRequest(T_REQ_DOMAIN_GET, utl::binaryPack(domain), dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  auto proof = utl::binaryUnpack<DomainProof>(result.value());
  if (!proof) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack the domain proof: " + proof.error().message);
  }
  return proof.value();
}

Client::Roe<Client::AccountProof> Client::fetchAccountProof(uint64_t accountId) {
  log().debug << "Requesting the proof of account " << accountId;
  auto result = sendRequest(T_REQ_ACCOUNT_PROOF, utl::binaryPack(accountId), dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  auto proof = utl::binaryUnpack<AccountProof>(result.value());
  if (!proof) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack the account proof: " + proof.error().message);
  }
  return proof.value();
}

Client::Roe<Client::ProducerStatsResponse> Client::fetchProducerStats(const ProducerStatsRequest &request) {
  log().debug << "Requesting producer stats: blocks " << request.fromBlockId << " to " << request.toBlockId;
  auto result = sendRequest(T_REQ_PRODUCER_STATS, utl::binaryPack(request), dataTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }
  auto response = utl::binaryUnpack<ProducerStatsResponse>(result.value());
  if (!response) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack producer stats: " + response.error().message);
  }
  return response.value();
}

Client::Roe<Client::TxGetByWalletResponse> Client::fetchTransactionsByWallet(const TxGetByWalletRequest &request) {
  log().debug << "Requesting transactions by wallet: " << request.walletId << " " << request.beforeBlockId;

  std::string payload = utl::binaryPack(request);
  auto result = sendRequest(T_REQ_TX_GET_BY_WALLET, payload, dataTimeout());

  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto responseResult = utl::binaryUnpack<TxGetByWalletResponse>(result.value());
  if (!responseResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack transactions by wallet response: " + responseResult.error().message);
  }
  return responseResult.value();
}

Client::Roe<Ledger::Record>
Client::fetchTransactionByIndex(const TxGetByIndexRequest &request) {
  log().debug << "Requesting transaction by index: " << request.txIndex;

  std::string payload = utl::binaryPack(request);
  auto result = sendRequest(T_REQ_TX_GET_BY_INDEX, payload, dataTimeout());

  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto responseResult = utl::binaryUnpack<Ledger::Record>(result.value());
  if (!responseResult) {
    return Error(E_INVALID_RESPONSE, "Failed to unpack transaction by index response: " + responseResult.error().message);
  }
  return responseResult.value();
}

Client::Roe<Client::TipReply> Client::addBlock(const Ledger::ChainNode& block) {
  log().debug << "Adding block " << block.block.index;

  std::string payload = block.ltsToString();
  auto result = sendRequest(T_REQ_BLOCK_ADD, payload, dataTimeout());
  if (!result) {
    return result.error();
  }
  return parseTipReply(result.value());
}

void Client::addBlockAsync(const Ledger::ChainNode &block, Done<TipReply> done, std::chrono::milliseconds timeout) {
  log().debug << "Adding block " << block.block.index;
  sendRequestAsync(T_REQ_BLOCK_ADD, block.ltsToString(), orDefault(timeout, dataTimeout()),
                   [done = std::move(done)](Roe<std::string> result) {
                     done(result ? parseTipReply(result.value()) : Roe<TipReply>(result.error()));
                   });
}

// MinerServer API - Transaction operations

Client::Roe<std::string> Client::submitTransaction(const Ledger::Record &record, std::chrono::milliseconds maxWait) {
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + maxWait;
  // The transaction's own window bounds it too: past it, no node would take it.
  if (auto typed = record.decode()) {
    std::visit(
        [&](const auto &tx) {
          using T = std::decay_t<decltype(tx)>;
          if constexpr (std::is_base_of_v<Ledger::TxIdempotencyWindow, T>) {
            if (tx.validationTsMax > 0) {
              const auto left = std::chrono::seconds(tx.validationTsMax) -
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch());
              deadline = std::min(deadline, Clock::now() + std::max(left, std::chrono::seconds(0)));
            }
          }
        },
        typed.value());
  }
  const std::string body = utl::binaryPack(record);
  std::mt19937 rng{std::random_device{}()};
  auto backoff = std::chrono::milliseconds(500);
  for (;;) {
    auto result = sendRequest(T_REQ_TX_ADD, body, dataTimeout());
    if (result || !isRetryable(result.error())) {
      return result;
    }
    auto wait = result.error().retryAfterMs > 0 ? std::chrono::milliseconds(result.error().retryAfterMs) : backoff;
    backoff = std::min(backoff * 2, std::chrono::milliseconds(10000));
    wait = std::chrono::milliseconds(
        static_cast<int64_t>(static_cast<double>(wait.count()) * std::uniform_real_distribution<double>(0.8, 1.2)(rng)));
    if (Clock::now() + wait >= deadline) {
      return result;
    }
    log().info << "Transaction not taken yet (" << result.error().message << "): sending it again in " << wait.count()
               << " ms";
    std::this_thread::sleep_for(wait);
  }
}

Client::Roe<void> Client::addTransaction(const Ledger::Record &record) {
  log().debug << "Adding transaction";

  std::string payload = utl::binaryPack(record);
  auto result = sendRequest(T_REQ_TX_ADD, payload, dataTimeout());
  if (!result) {
    return result.error();
  }

  return {};
}

void Client::pullTransactionsAsync(uint64_t slot, Done<std::vector<Ledger::Record>> done,
                                   std::chrono::milliseconds timeout) {
  sendRequestAsync(T_REQ_TX_PULL, utl::binaryPack(slot), orDefault(timeout, dataTimeout()),
                   [done = std::move(done)](Roe<std::string> result) {
    if (!result) {
      done(Roe<std::vector<Ledger::Record>>(result.error()));
      return;
    }
    auto records = utl::binaryUnpack<std::vector<Ledger::Record>>(result.value());
    done(records ? Roe<std::vector<Ledger::Record>>(std::move(records.value()))
                 : Roe<std::vector<Ledger::Record>>(Error(E_INVALID_RESPONSE, "Invalid transaction pull reply")));
  });
}

Client::Roe<std::string> Client::addEquivocation(const Equivocation &evidence) {
  return sendRequest(T_REQ_EQUIVOCATION_ADD, evidence.ltsToString(), dataTimeout());
}

void Client::addEquivocationAsync(const Equivocation &evidence, Done<std::string> done,
                                  std::chrono::milliseconds timeout) {
  sendRequestAsync(T_REQ_EQUIVOCATION_ADD, evidence.ltsToString(), orDefault(timeout, dataTimeout()), std::move(done));
}

void Client::addTransactionAsync(const Ledger::Record &record, Done<void> done, std::chrono::milliseconds timeout) {
  sendRequestAsync(T_REQ_TX_ADD, utl::binaryPack(record), orDefault(timeout, dataTimeout()),
                   [done = std::move(done)](Roe<std::string> result) {
                     done(result ? Roe<void>() : Roe<void>(result.error()));
                   });
}

// MinerServer API - Status

Client::Roe<Client::MinerStatus> Client::fetchMinerStatus() {
  log().debug << "Requesting miner status";

  auto result = sendRequest(T_REQ_STATUS, "", fastTimeout());
  if (!result) {
    return Error(result.error().code, result.error().message);
  }

  auto metaResult = utl::binaryUnpack<pp::common::Meta>(result.value());
  if (!metaResult) {
    return Error(E_INVALID_RESPONSE,
                 "Failed to unpack miner status Meta: " +
                     metaResult.error().message);
  }
  MinerStatus status;
  auto parseResult = status.ltsFromMeta(metaResult.value());
  if (!parseResult) {
    return Error(parseResult.error().code, parseResult.error().message);
  }
  return status;
}

} // namespace pp
