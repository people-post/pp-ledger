#ifndef PP_LEDGER_ENV_CONFIG_H
#define PP_LEDGER_ENV_CONFIG_H

#include "common/ResultOrError.hpp"
#include "common/Value.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pp::env_config {

/**
 * Configuration from the environment (docs/ops/CONFIGURATION.md § Environment). Every `config.json`
 * field a deployment sets has a `PP_LEDGER_*` variable; set, it wins over the file, and a node whose
 * required fields all come from the environment needs no file at all. Values are written into the
 * JSON object the file parses into, so they pass the same validation.
 */
struct Error : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};
template <typename T> using Roe = ResultOrError<T, Error>;

/** How a variable's text becomes a JSON value. */
enum class Kind {
  String,
  UInt, ///< decimal
  List, ///< strings separated by commas or newlines (blanks around items dropped)
};

/** A variable and the config field it sets (`parent`: a nested object, e.g. networkAnchor). */
struct Field {
  std::string env;
  std::string key;
  Kind kind{Kind::String};
  std::string parent;
  /** A secret: also read from `<env>_FILE` (secret()). */
  bool secret{false};
};

/** The variable's value; none when unset or empty. */
std::optional<std::string> get(const std::string &name);

/**
 * A secret: `NAME` holds it, or `NAME_FILE` names a file holding it (Docker / Kubernetes secrets;
 * surrounding whitespace dropped). Both set is an error; none if neither is.
 */
Roe<std::optional<std::string>> secret(const std::string &name);

/** Write each set variable over its field in `config`. Returns how many were set. */
Roe<size_t> overlay(pp::common::Object &config, const std::vector<Field> &fields);

/**
 * A node's run config: `path` (config.json) if present, overlaid with the variables in `fields`.
 * None when neither the file nor any of the variables is there (the caller writes its template).
 */
Roe<std::optional<pp::common::Object>> loadConfig(const std::string &path, const std::vector<Field> &fields);

/** Fields every role reads: `PP_LEDGER_PORT`, `PP_LEDGER_ALLOWED_PEERS`, `PP_LEDGER_MAX_LINKS`, `PP_LEDGER_HANDSHAKE_BACKLOG`. */
std::vector<Field> commonFields();
/** The network anchor (relay, miner): `PP_LEDGER_NETWORK_ID`, `PP_LEDGER_GENESIS_HASH`,
 *  `PP_LEDGER_TRUSTED_CHECKPOINT`, `PP_LEDGER_PREDECESSOR_DIR`. */
std::vector<Field> anchorFields();

} // namespace pp::env_config

#endif // PP_LEDGER_ENV_CONFIG_H
