#include "AddonUtils.h"

#include <cmath>

namespace pp {
namespace nodeaddon {

namespace {

constexpr uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL;

} // namespace

uint64_t ValueToUint64(const Napi::Env& env, const Napi::Value& value, const char* fieldName) {
  if (value.IsBigInt()) {
    bool lossless = false;
    uint64_t parsed = value.As<Napi::BigInt>().Uint64Value(&lossless);
    if (!lossless) {
      throw Napi::TypeError::New(env, std::string(fieldName) + " must be a lossless uint64 BigInt");
    }
    return parsed;
  }

  if (value.IsNumber()) {
    double parsed = value.As<Napi::Number>().DoubleValue();
    if (parsed < 0 || parsed > static_cast<double>(MAX_SAFE_INTEGER) || std::floor(parsed) != parsed) {
      throw Napi::TypeError::New(env, std::string(fieldName) + " must be a non-negative integer <= Number.MAX_SAFE_INTEGER");
    }
    return static_cast<uint64_t>(parsed);
  }

  throw Napi::TypeError::New(env, std::string(fieldName) + " must be a Number or BigInt");
}

Napi::Value JsonStringToJsValue(const Napi::Env& env, const std::string& jsonString) {
  try {
    Napi::Object global = env.Global();
    Napi::Object jsonObj = global.Get("JSON").As<Napi::Object>();
    Napi::Function parseFn = jsonObj.Get("parse").As<Napi::Function>();
    return parseFn.Call(jsonObj, {Napi::String::New(env, jsonString)});
  } catch (const std::exception& e) {
    throw Napi::Error::New(env, std::string("Failed to parse JSON result: ") + e.what());
  }
}

JsonAsyncWorker::JsonAsyncWorker(const Napi::Env& env, WorkFn fn)
    : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), fn_(std::move(fn)) {}

void JsonAsyncWorker::Execute() {
  if (!fn_(resultJson_, errorMessage_)) {
    SetError(errorMessage_);
  }
}

void JsonAsyncWorker::OnOK() {
  Napi::HandleScope scope(Env());
  try {
    deferred_.Resolve(JsonStringToJsValue(Env(), resultJson_));
  } catch (const std::exception& e) {
    deferred_.Reject(Napi::Error::New(Env(), e.what()).Value());
  }
}

void JsonAsyncWorker::OnError(const Napi::Error& e) {
  Napi::HandleScope scope(Env());
  deferred_.Reject(e.Value());
}

Napi::Promise JsonAsyncWorker::Promise() const {
  return deferred_.Promise();
}

} // namespace nodeaddon
} // namespace pp
