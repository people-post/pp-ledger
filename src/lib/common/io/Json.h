#pragma once

#include "Meta.h"
#include "ResultOrError.hpp"

#include "common/Error.h"

#include <string>

namespace pp::common::io {

/** Serialize any Value root to JSON (UTF-8). indent < 0 → compact. */
pp::Roe<std::string> valueToJsonString(const Value &v, int indent = -1);

/** Parse JSON text into a Value (object, array, scalar, or null). */
pp::Roe<Value> valueFromJsonString(const std::string &json);

/**
 * Serialize Object to JSON. Convenience for Meta / ltsToMeta call sites.
 * indent < 0 → compact; indent >= 0 → pretty-print.
 * On encode failure (e.g. u64 > INT64_MAX), returns a small JSON error object.
 */
std::string metaToJsonString(const Meta &m, int indent = -1);

/**
 * Parse a JSON object into Meta. Returns false on syntax/type mismatch or
 * non-object root. Prefer valueFromJsonString for structured errors.
 */
bool metaFromJsonString(Meta &out, const std::string &json);

} // namespace pp::common::io
