#pragma once

/**
 * Ledger-only alias: Meta is Object (human intermediate tree for ltsToMeta).
 * Implementation lives in pp-cpp-common as Value/Object.
 */
#include "common/Value.h"

namespace pp::common {

using Meta = Object;
using MetaPtr = ObjectPtr;

} // namespace pp::common
