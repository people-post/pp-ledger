#ifndef PP_LEDGER_NODE_FILE_H
#define PP_LEDGER_NODE_FILE_H

#include "common/ResultOrError.hpp"

#include <optional>
#include <string>

namespace pp::node_file {

/**
 * Small node-local state files beside the ledger (signed tip, beacon certificate, equivocation
 * evidence): read whole, replaced atomically, removed. Callers map errors to their own codes.
 */
struct Error : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};
template <typename T> using Roe = ResultOrError<T, Error>;

/** The file's bytes; none when it does not exist. The file is closed on return. */
Roe<std::optional<std::string>> readIfExists(const std::string &path);
/**
 * Replace `path` with `bytes` durably (utl::replaceFile): after a crash or a power loss it holds the
 * old bytes or the new.
 */
Roe<void> replace(const std::string &path, const std::string &bytes);
/** Remove `path` if present. */
Roe<void> remove(const std::string &path);

} // namespace pp::node_file

#endif // PP_LEDGER_NODE_FILE_H
