#ifndef PP_LEDGER_BINARY_PACK_H
#define PP_LEDGER_BINARY_PACK_H

#include "Serializer.h"
#include "ResultOrError.hpp"
#include <string>

namespace pp {
namespace utl {

// Error type for binary unpack operations
struct BinaryUnpackError : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
};

/**
 * Pack a struct/object to binary string using Serializer
 * @param t The object to serialize
 * @return Binary string representation
 */
template<typename T>
std::string binaryPack(const T& t) {
    return Serializer::serialize(t);
}

/**
 * Unpack a binary string to a struct/object using Serializer
 * @param data Binary string data
 * @return ResultOrError containing the deserialized object or an error
 */
template<typename T>
ResultOrError<T, BinaryUnpackError> binaryUnpack(const std::string& data) {
    T result;
    if (!Serializer::deserialize(data, result)) {
        return BinaryUnpackError(1, "Failed to deserialize binary data");
    }
    return result;
}

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_BINARY_PACK_H
