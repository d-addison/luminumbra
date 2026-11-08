#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Luminumbra::Persistence::detail {

// Internal test seams. Install before saving; reset only after the save returns.
// Zero selects the hardware limit. Explicit limits are still capped at eight.
void set_chunk_encode_workers_for_testing(unsigned workers);
// Overrides the bytes of in-flight encoding the workers may hold together; zero restores the
// default.
void set_chunk_encode_in_flight_limit_for_testing(std::size_t bytes);
// Called inside the encoder, before serialization; exceptions follow normal worker handling.
using ChunkEncodeHook = void (*)(std::size_t index);
void set_chunk_encode_hook_for_testing(ChunkEncodeHook hook);

// Same bytes as value.dump(indent) (compact for a negative indent), with the serializer built
// under the lock every record encoder and snapshot writer shares. Use it for JSON written while a
// save may be encoding on worker threads.
std::string dump_json_guarded(const nlohmann::json& value, int indent = -1);

} // namespace Luminumbra::Persistence::detail
