#pragma once

#include "Meta.h"

#include <string>

namespace pp::common::io {

/** Serialize Meta to a compact JSON object string (no nlohmann). */
std::string metaToJsonString(const Meta &m);

/** Parse JSON object into Meta. Returns false on syntax/type mismatch. */
bool metaFromJsonString(Meta &out, const std::string &json);

} // namespace pp::common::io
