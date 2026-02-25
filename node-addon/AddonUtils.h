#pragma once

#include <napi.h>

#include <cstdint>
#include <functional>
#include <string>

namespace pp {
namespace nodeaddon {

uint64_t ValueToUint64(const Napi::Env& env, const Napi::Value& value, const char* fieldName);
Napi::Value JsonStringToJsValue(const Napi::Env& env, const std::string& jsonString);

class JsonAsyncWorker : public Napi::AsyncWorker {
public:
  using WorkFn = std::function<bool(std::string&, std::string&)>;

  JsonAsyncWorker(const Napi::Env& env, WorkFn fn);

  void Execute() override;
  void OnOK() override;
  void OnError(const Napi::Error& e) override;

  Napi::Promise Promise() const;

private:
  Napi::Promise::Deferred deferred_;
  WorkFn fn_;
  std::string resultJson_;
  std::string errorMessage_;
};

} // namespace nodeaddon
} // namespace pp
