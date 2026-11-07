#pragma once

#include <cstddef>

namespace Luminumbra::Persistence::detail {

// Internal test seams. Install before saving; reset only after the save returns.
// Zero selects the hardware limit. Explicit limits are still capped at eight.
void set_chunk_encode_workers_for_testing(unsigned workers);
// Called inside the encoder, before serialization; exceptions follow normal worker handling.
using ChunkEncodeHook = void (*)(std::size_t index);
void set_chunk_encode_hook_for_testing(ChunkEncodeHook hook);

} // namespace Luminumbra::Persistence::detail
