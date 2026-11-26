#pragma once

#include <cstdint>

namespace Luminumbra::Persistence::detail {
// Process-wide load_world_impl attempts, including refusals and worker loads.
// Monotonic observation only: take differences after joining workers. Declared
// only in test support to keep this accessor off the public persistence facade.
std::uint64_t decode_count_for_testing();
} // namespace Luminumbra::Persistence::detail
