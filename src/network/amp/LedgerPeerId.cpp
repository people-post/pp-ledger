#include "LedgerPeerId.h"

#include "crypto/MlDsa.h"

#include <sodium.h>

#include <array>
#include <cassert>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace pp {
namespace network {
namespace {

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

enum class HashType : uint64_t { kIdentity = 0, kSha256 = 18 };

void AppendVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

void AppendTag(std::vector<uint8_t>& out, uint32_t field_number, uint8_t wire_type) {
  AppendVarint(out, (static_cast<uint64_t>(field_number) << 3) | wire_type);
}

void AppendBytesField(std::vector<uint8_t>& out, uint32_t field_number, const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return;
  }
  AppendTag(out, field_number, 2);
  AppendVarint(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}

std::vector<uint8_t> EncodeMlDsa65PublicKeyWire(const std::vector<uint8_t>& public_key) {
  std::vector<uint8_t> out;
  out.reserve(public_key.size() + 16);
  AppendTag(out, 1, 0);
  AppendVarint(out, 4);
  AppendBytesField(out, 2, public_key);
  return out;
}

std::vector<uint8_t> BuildMultihashBytes(HashType type, std::span<const uint8_t> hash) {
  std::vector<uint8_t> bytes;
  bytes.reserve(hash.size() + 4);
  AppendVarint(bytes, static_cast<uint64_t>(type));
  assert(hash.size() <= std::numeric_limits<uint8_t>::max());
  bytes.push_back(static_cast<uint8_t>(hash.size()));
  bytes.insert(bytes.end(), hash.begin(), hash.end());
  return bytes;
}

std::string EncodeBase58(std::span<const uint8_t> bytes) {
  int zeroes = 0;
  while (zeroes < static_cast<int>(bytes.size()) && bytes[static_cast<size_t>(zeroes)] == 0) {
    ++zeroes;
  }

  const auto* begin = bytes.data() + zeroes;
  const auto* end = bytes.data() + bytes.size();
  const int size = static_cast<int>((end - begin) * 138 / 100 + 1);
  std::vector<unsigned char> encoded(static_cast<size_t>(size), 0);
  int length = 0;

  for (const auto* p = begin; p != end; ++p) {
    int carry = *p;
    int i = 0;
    for (auto it = encoded.rbegin(); (carry != 0 || i < length) && it != encoded.rend(); ++it, ++i) {
      carry += 256 * (*it);
      *it = static_cast<unsigned char>(carry % 58);
      carry /= 58;
    }
    length = i;
  }

  auto it = encoded.begin() + (size - length);
  while (it != encoded.end() && *it == 0) {
    ++it;
  }

  std::string out;
  out.reserve(static_cast<size_t>(zeroes) + static_cast<size_t>(encoded.end() - it));
  out.assign(static_cast<size_t>(zeroes), '1');
  while (it != encoded.end()) {
    out.push_back(kBase58Alphabet[*it]);
    ++it;
  }
  return out;
}

constexpr size_t kMaxInlineKeyLength = 42;

std::vector<uint8_t> PeerIdBytesFromProtobufPublicKey(std::span<const uint8_t> protobuf_key) {
  if (protobuf_key.size() <= kMaxInlineKeyLength) {
    return BuildMultihashBytes(HashType::kIdentity,
                               std::vector<uint8_t>(protobuf_key.begin(), protobuf_key.end()));
  }

  std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
  if (crypto_hash_sha256(digest.data(), protobuf_key.data(), protobuf_key.size()) != 0) {
    return {};
  }
  return BuildMultihashBytes(HashType::kSha256, digest);
}

} // namespace

pp::Roe<std::string> PeerIdFromMlDsaPublicKey(const std::vector<uint8_t>& public_key) {
  if (public_key.size() != pp::kMlDsa65PublicKeyBytes) {
    return pp::Error("ML-DSA-65 public key must be 1952 bytes");
  }
  const auto wire = EncodeMlDsa65PublicKeyWire(public_key);
  if (wire.empty()) {
    return pp::Error("Failed to encode ML-DSA-65 public key");
  }
  const auto multihash = PeerIdBytesFromProtobufPublicKey(wire);
  if (multihash.empty()) {
    return pp::Error("Failed to derive peer id multihash");
  }
  return EncodeBase58(multihash);
}

} // namespace network
} // namespace pp
