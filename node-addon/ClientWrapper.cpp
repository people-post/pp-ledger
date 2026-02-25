#include "ClientWrapper.h"

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

Napi::Value ClientWrapper::queueJson(const Napi::Env& env, JsonAsyncWorker::WorkFn fn) {
  auto* worker = new JsonAsyncWorker(env, std::move(fn));
  worker->Queue();
  return worker->Promise();
}

} // namespace nodeaddon
} // namespace pp
