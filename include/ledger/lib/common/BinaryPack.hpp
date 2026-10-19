#pragma once

#include "common/BinaryPack.hpp"

namespace pp {
namespace utl {

/** Compat wrappers: ledger call sites use pp::utl::*; implementation is pp::. */
template <typename T> std::string binaryPack(const T &t) {
  return ::pp::binaryPack(t);
}

template <typename T> Roe<T> binaryUnpack(const std::string &data) {
  return ::pp::binaryUnpack<T>(data);
}

} // namespace utl
} // namespace pp
