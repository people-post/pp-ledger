#ifndef PP_LEDGER_BINARY_PACK_H
#define PP_LEDGER_BINARY_PACK_H

#include "Serializer.h"
#include <string>
#include <stdexcept>

namespace pp {
namespace utl {

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
 * @return Deserialized object
 * @throws std::runtime_error if deserialization fails
 */
template<typename T>
T binaryUnpack(const std::string& data) {
    T result;
    if (!Serializer::deserialize(data, result)) {
        throw std::runtime_error("Failed to deserialize binary data");
    }
    return result;
}

} // namespace utl
} // namespace pp

#endif // PP_LEDGER_BINARY_PACK_H
