#ifndef PP_LEDGER_BINARY_PACK_HPP
#define PP_LEDGER_BINARY_PACK_HPP

#include "ResultOrError.hpp"
#include "Serialize.hpp"
#include <sstream>
#include <string>

namespace pp {
namespace utl {

// Error type for binary unpack operations
struct BinaryUnpackError : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};

/**
 * Pack a struct/object to binary string using OutputArchive
 * @param t The object to serialize
 * @return Binary string representation
 */
template <typename T> std::string binaryPack(const T &t) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar &t;
  return oss.str();
}

/**
 * Unpack a binary string to a struct/object using InputArchive
 * @param data Binary string data
 * @return ResultOrError containing the deserialized object or an error
 */
template <typename T>
ResultOrError<T, BinaryUnpackError> binaryUnpack(const std::string &data) {
  std::istringstream iss(data);
  InputArchive ar(iss);
  T result;
  ar &result;
  if (ar.failed()) {
    return BinaryUnpackError(1, "Failed to deserialize binary data");
  }
  return result;
}

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_BINARY_PACK_HPP
