#pragma once

#include "Meta.h"

#include <string>

namespace pp::common::io {

/**
 * Serialize Meta to JSON (no nlohmann).
 * If indent < 0, output is compact. If indent >= 0, pretty-print with that many spaces per nesting level.
 */
std::string metaToJsonString(const Meta &m, int indent = -1);

/** Parse JSON object into Meta. Returns false on syntax/type mismatch. */
bool metaFromJsonString(Meta &out, const std::string &json);

} // namespace pp::common::io
