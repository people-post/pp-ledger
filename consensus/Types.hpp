#pragma once

#include <cstdint>

namespace pp {
namespace consensus {

struct Stakeholder {
  uint64_t id{ 0 };
  uint64_t stake{ 0 };
};

} // namespace consensus
} // namespace pp
