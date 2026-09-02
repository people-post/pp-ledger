#pragma once

#include <cstddef>

namespace pp {
namespace network {

struct PerformanceConfig {
  size_t handlerWorkers{4};
};

} // namespace network
} // namespace pp
