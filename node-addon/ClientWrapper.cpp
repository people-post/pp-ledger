#include "ClientWrapper.h"

#include "../lib/BinaryPack.hpp"
#include "../lib/Utilities.h"
#include <chrono>
#include <cctype>
#include <random>

namespace {

class VoidAsyncWorker : public Napi::AsyncWorker {
public:
  using WorkFn = std::function<bool(std::string&)>;

  VoidAsyncWorker(const Napi::Env& env, WorkFn fn)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), fn_(std::move(fn)) {}

  void Execute() override {
    if (!fn_(errorMessage_)) {
      SetError(errorMessage_);
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    deferred_.Resolve(Napi::Boolean::New(Env(), true));
  }

  void OnError(const Napi::Error& e) override {
    Napi::HandleScope scope(Env());
    deferred_.Reject(e.Value());
  }

  Napi::Promise Promise() const {
    return deferred_.Promise();
  }

private:
  Napi::Promise::Deferred deferred_;
  WorkFn fn_;
  std::string errorMessage_;
};

bool isHexStringStrict(const std::string& input) {
  if (input.empty() || (input.size() % 2) != 0) {
    return false;
  }
  for (unsigned char ch : input) {
    if (!std::isxdigit(ch)) {
      return false;
    }
  }
  return true;
}

uint64_t randomUInt64() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dist;
  return dist(gen);
}

uint64_t parseStrictHexField(const Napi::Env& env, const Napi::Object& obj, const char* key, bool required) {
  if (!obj.Has(key)) {
    if (required) {
      throw Napi::TypeError::New(env, std::string("request.") + key + " is required");
    }
    return 0;
  }
  Napi::Value value = obj.Get(key);
  return pp::nodeaddon::ValueToUint64(env, value, key);
}

} // namespace

namespace pp {
namespace nodeaddon {

Napi::Object ClientWrapper::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function ctor = DefineClass(
    env,
    "Client",
    {
      InstanceMethod("setEndpoint", &ClientWrapper::SetEndpoint),
      InstanceMethod("fetchBeaconState", &ClientWrapper::FetchBeaconState),
      InstanceMethod("fetchCalibration", &ClientWrapper::FetchCalibration),
      InstanceMethod("fetchMinerList", &ClientWrapper::FetchMinerList),
      InstanceMethod("fetchMinerStatus", &ClientWrapper::FetchMinerStatus),
      InstanceMethod("fetchBlock", &ClientWrapper::FetchBlock),
      InstanceMethod("fetchUserAccount", &ClientWrapper::FetchUserAccount),
      InstanceMethod("fetchTransactionsByWallet", &ClientWrapper::FetchTransactionsByWallet),
      InstanceMethod("fetchTransactionByIndex", &ClientWrapper::FetchTransactionByIndex),
      InstanceMethod("buildTransactionHex", &ClientWrapper::BuildTransactionHex),
      InstanceMethod("addTransaction", &ClientWrapper::AddTransaction),
    }
  );

  constructor() = Napi::Persistent(ctor);
  constructor().SuppressDestruct();

  exports.Set("Client", ctor);
  return exports;
}

ClientWrapper::ClientWrapper(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ClientWrapper>(info) {
  Napi::Env env = info.Env();

  if (info.Length() == 0) {
    return;
  }

  if (!info[0].IsString()) {
    throw Napi::TypeError::New(env, "Client constructor expects optional endpoint string");
  }

  auto result = client_.setEndpoint(info[0].As<Napi::String>().Utf8Value());
  if (!result) {
    throw Napi::Error::New(env, result.error().message);
  }
}

Napi::FunctionReference& ClientWrapper::constructor() {
  static Napi::FunctionReference ctor;
  return ctor;
}

Napi::Value ClientWrapper::SetEndpoint(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() != 1 || !info[0].IsString()) {
    throw Napi::TypeError::New(env, "setEndpoint(endpoint) expects a single endpoint string");
  }

  auto result = client_.setEndpoint(info[0].As<Napi::String>().Utf8Value());
  if (!result) {
    throw Napi::Error::New(env, result.error().message);
  }

  return env.Undefined();
}

Napi::Value ClientWrapper::FetchBeaconState(const Napi::CallbackInfo& info) {
  return queueJson(info.Env(), [this](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchBeaconState();
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().ltsToJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchCalibration(const Napi::CallbackInfo& info) {
  return queueJson(info.Env(), [this](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchCalibration();
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().toJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchMinerList(const Napi::CallbackInfo& info) {
  return queueJson(info.Env(), [this](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchMinerList();
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }

    nlohmann::json miners = nlohmann::json::array();
    for (const auto& miner : result.value()) {
      miners.push_back(miner.ltsToJson());
    }
    outJson = miners.dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchMinerStatus(const Napi::CallbackInfo& info) {
  return queueJson(info.Env(), [this](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchMinerStatus();
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().ltsToJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchBlock(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1) {
    throw Napi::TypeError::New(env, "fetchBlock(blockId) expects one argument");
  }

  uint64_t blockId = ValueToUint64(env, info[0], "blockId");

  return queueJson(env, [this, blockId](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchBlock(blockId);
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().toJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchUserAccount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1) {
    throw Napi::TypeError::New(env, "fetchUserAccount(accountId) expects one argument");
  }

  uint64_t accountId = ValueToUint64(env, info[0], "accountId");

  return queueJson(env, [this, accountId](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchUserAccount(accountId);
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().toJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchTransactionsByWallet(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "fetchTransactionsByWallet(request) expects an object");
  }

  Napi::Object requestObj = info[0].As<Napi::Object>();
  if (!requestObj.Has("walletId")) {
    throw Napi::TypeError::New(env, "request.walletId is required");
  }

  uint64_t walletId = ValueToUint64(env, requestObj.Get("walletId"), "walletId");
  uint64_t beforeBlockId = 0;
  if (requestObj.Has("beforeBlockId")) {
    beforeBlockId = ValueToUint64(env, requestObj.Get("beforeBlockId"), "beforeBlockId");
  }

  Client::TxGetByWalletRequest request;
  request.walletId = walletId;
  request.beforeBlockId = beforeBlockId;

  return queueJson(env, [this, request](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchTransactionsByWallet(request);
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().toJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::FetchTransactionByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "fetchTransactionByIndex(request) expects an object");
  }

  Napi::Object requestObj = info[0].As<Napi::Object>();
  if (!requestObj.Has("txIndex")) {
    throw Napi::TypeError::New(env, "request.txIndex is required");
  }

  uint64_t txIndex = ValueToUint64(env, requestObj.Get("txIndex"), "txIndex");

  Client::TxGetByIndexRequest request;
  request.txIndex = txIndex;

  return queueJson(env, [this, request](std::string& outJson, std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.fetchTransactionByIndex(request);
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    outJson = result.value().toJson().dump();
    return true;
  });
}

Napi::Value ClientWrapper::BuildTransactionHex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "buildTransactionHex(request) expects one object argument");
  }

  Napi::Object req = info[0].As<Napi::Object>();

  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj.type = static_cast<uint16_t>(parseStrictHexField(env, req, "type", false));
  signedTx.obj.tokenId = parseStrictHexField(env, req, "tokenId", false);
  signedTx.obj.fromWalletId = parseStrictHexField(env, req, "fromWalletId", true);
  signedTx.obj.toWalletId = parseStrictHexField(env, req, "toWalletId", true);
  signedTx.obj.amount = parseStrictHexField(env, req, "amount", true);
  signedTx.obj.fee = parseStrictHexField(env, req, "fee", false);

  if (req.Has("metaHex")) {
    if (!req.Get("metaHex").IsString()) {
      throw Napi::TypeError::New(env, "request.metaHex must be a hex string when provided");
    }
    std::string metaHex = req.Get("metaHex").As<Napi::String>().Utf8Value();
    if (!metaHex.empty()) {
      if (!isHexStringStrict(metaHex)) {
        throw Napi::TypeError::New(env, "request.metaHex must be an even-length hex string without 0x prefix");
      }
      signedTx.obj.meta = pp::utl::hexDecode(metaHex);
      if (signedTx.obj.meta.empty()) {
        throw Napi::TypeError::New(env, "request.metaHex failed to decode");
      }
    }
  }

  if (req.Has("idempotentId")) {
    signedTx.obj.idempotentId = parseStrictHexField(env, req, "idempotentId", false);
  } else {
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    signedTx.obj.idempotentId = static_cast<uint64_t>(now) ^ (randomUInt64() & 0xFFFFULL);
    if (signedTx.obj.idempotentId == 0) {
      signedTx.obj.idempotentId = 1;
    }
  }

  if (req.Has("validationTsMin")) {
    signedTx.obj.validationTsMin = static_cast<int64_t>(ValueToUint64(env, req.Get("validationTsMin"), "validationTsMin"));
  }
  if (req.Has("validationTsMax")) {
    signedTx.obj.validationTsMax = static_cast<int64_t>(ValueToUint64(env, req.Get("validationTsMax"), "validationTsMax"));
  }
  if (!req.Has("validationTsMin") && !req.Has("validationTsMax")) {
    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    signedTx.obj.validationTsMin = now - 60;
    signedTx.obj.validationTsMax = now + 3600;
  }

  // External signing flow: return unsigned transaction bytes only.
  std::string unsignedTxPayload = pp::utl::binaryPack(signedTx.obj);
  return Napi::String::New(env, pp::utl::hexEncode(unsignedTxPayload));
}

Napi::Value ClientWrapper::AddTransaction(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsObject()) {
    throw Napi::TypeError::New(env, "addTransaction(request) expects one object argument");
  }

  Napi::Object req = info[0].As<Napi::Object>();
  if (!req.Has("transactionHex") || !req.Get("transactionHex").IsString()) {
    throw Napi::TypeError::New(env, "request.transactionHex is required and must be a hex string");
  }

  if (!req.Has("signaturesHex") || !req.Get("signaturesHex").IsArray()) {
    throw Napi::TypeError::New(env, "request.signaturesHex is required and must be an array of hex strings");
  }

  std::string transactionHex = req.Get("transactionHex").As<Napi::String>().Utf8Value();
  if (!isHexStringStrict(transactionHex)) {
    throw Napi::TypeError::New(env, "request.transactionHex must be a non-empty even-length hex string without 0x prefix");
  }
  std::string transactionPayload = pp::utl::hexDecode(transactionHex);
  if (transactionPayload.empty()) {
    throw Napi::TypeError::New(env, "request.transactionHex failed to decode");
  }

  auto txUnpacked = pp::utl::binaryUnpack<Ledger::Transaction>(transactionPayload);
  if (!txUnpacked) {
    throw Napi::TypeError::New(env, std::string("Invalid packed transactionHex: ") + txUnpacked.error().message);
  }

  Ledger::SignedData<Ledger::Transaction> signedTx;
  signedTx.obj = txUnpacked.value();

  Napi::Array signaturesArray = req.Get("signaturesHex").As<Napi::Array>();
  if (signaturesArray.Length() == 0) {
    throw Napi::TypeError::New(env, "request.signaturesHex must contain at least one signature");
  }

  signedTx.signatures.reserve(signaturesArray.Length());
  for (uint32_t i = 0; i < signaturesArray.Length(); ++i) {
    Napi::Value item = signaturesArray.Get(i);
    if (!item.IsString()) {
      throw Napi::TypeError::New(env, "request.signaturesHex entries must be strings");
    }
    std::string sigHex = item.As<Napi::String>().Utf8Value();
    if (!isHexStringStrict(sigHex) || sigHex.size() != 128) {
      throw Napi::TypeError::New(env, "each signature hex must be exactly 128 hex chars (64 bytes), without 0x prefix");
    }
    std::string sig = pp::utl::hexDecode(sigHex);
    if (sig.size() != 64) {
      throw Napi::TypeError::New(env, "signature hex failed to decode to 64 bytes");
    }
    signedTx.signatures.push_back(std::move(sig));
  }

  auto* worker = new VoidAsyncWorker(env, [this, signedTx](std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = client_.addTransaction(signedTx);
    if (!result) {
      errorMessage = result.error().message;
      return false;
    }
    return true;
  });
  worker->Queue();
  return worker->Promise();
}

Napi::Value ClientWrapper::queueJson(const Napi::Env& env, JsonAsyncWorker::WorkFn fn) {
  auto* worker = new JsonAsyncWorker(env, std::move(fn));
  worker->Queue();
  return worker->Promise();
}

} // namespace nodeaddon
} // namespace pp
