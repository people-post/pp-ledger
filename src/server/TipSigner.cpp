#include "TipSigner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace pp {

TipSigner::TipSigner(std::string path, std::vector<utl::MlDsaKeyPair> keys)
    : path_(std::move(path)), keys_(std::move(keys)) {
  redirectLogger("TipSigner");
}

TipSigner::Roe<void> TipSigner::load() {
  latest_.reset();
  if (!std::filesystem::exists(path_)) {
    return {};
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    return Error(E_IO, "Failed to read " + path_);
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Client::SignedTip tip;
  if (!tip.ltsFromString(bytes)) {
    return Error(E_IO, path_ + " is not a signed tip");
  }
  latest_ = std::move(tip);
  return {};
}

bool TipSigner::holdsKey(const std::string &publicKey) const {
  return std::any_of(keys_.begin(), keys_.end(), [&](const auto &key) { return key.publicKey == publicKey; });
}

TipSigner::Roe<void> TipSigner::sign(const BeaconCertificate &cert, uint64_t height, const std::string &blockHash) {
  if (latest_ && height <= latest_->height) {
    if (height == latest_->height && blockHash == latest_->blockHash) {
      return {};
    }
    return Error(E_CONFLICT, "Refusing to sign block " + std::to_string(height) +
                                 ": the beacon already signed a different block at or above it (height " +
                                 std::to_string(latest_->height) + ")");
  }
  const auto key = std::find_if(keys_.begin(), keys_.end(),
                                [&](const auto &held) { return held.publicKey == cert.publicKey; });
  if (key == keys_.end()) {
    return Error(E_NOT_CERTIFIED, "Beacon generation " + std::to_string(cert.generation) +
                                      " certifies a key this beacon does not hold");
  }
  Client::SignedTip tip;
  tip.networkId = cert.networkId;
  tip.generation = cert.generation;
  tip.height = height;
  tip.blockHash = blockHash;
  auto signature = utl::mlDsaSign(key->privateKey, tip.signingMessage());
  if (!signature) {
    return Error(E_IO, "Failed to sign tip: " + signature.error().message);
  }
  tip.signature = signature.value();
  if (auto stored = persist(tip); !stored) {
    return stored.error();
  }
  latest_ = std::move(tip);
  return {};
}

TipSigner::Roe<void> TipSigner::persist(const Client::SignedTip &tip) const {
  // Write aside, then rename over: a crash leaves the old tip or the new one, never half of one.
  const std::string temp = path_ + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << tip.ltsToString();
    out.flush();
    if (!out) {
      return Error(E_IO, "Failed to write " + temp);
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path_, ec);
  if (ec) {
    return Error(E_IO, "Failed to replace " + path_ + ": " + ec.message());
  }
  return {};
}

} // namespace pp
