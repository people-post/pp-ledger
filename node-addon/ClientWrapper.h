#pragma once

#include "AddonUtils.h"
#include "../client/Client.h"

#include <napi.h>

#include <mutex>

namespace pp {
namespace nodeaddon {

class ClientWrapper : public Napi::ObjectWrap<ClientWrapper> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);

  explicit ClientWrapper(const Napi::CallbackInfo& info);

private:
  static Napi::FunctionReference& constructor();

  Napi::Value SetEndpoint(const Napi::CallbackInfo& info);
  Napi::Value FetchBeaconState(const Napi::CallbackInfo& info);
  Napi::Value FetchCalibration(const Napi::CallbackInfo& info);
  Napi::Value FetchMinerList(const Napi::CallbackInfo& info);
  Napi::Value FetchMinerStatus(const Napi::CallbackInfo& info);
  Napi::Value FetchBlock(const Napi::CallbackInfo& info);
  Napi::Value FetchUserAccount(const Napi::CallbackInfo& info);
  Napi::Value FetchTransactionsByWallet(const Napi::CallbackInfo& info);

  Napi::Value queueJson(const Napi::Env& env, JsonAsyncWorker::WorkFn fn);

  Client client_;
  std::mutex mutex_;
};

} // namespace nodeaddon
} // namespace pp
