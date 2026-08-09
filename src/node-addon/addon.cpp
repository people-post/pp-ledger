#include "ClientWrapper.h"

namespace pp {
namespace nodeaddon {

Napi::Object InitAddon(Napi::Env env, Napi::Object exports) {
  return ClientWrapper::Init(env, exports);
}

NODE_API_MODULE(pp_client_node, InitAddon)

} // namespace nodeaddon
} // namespace pp
