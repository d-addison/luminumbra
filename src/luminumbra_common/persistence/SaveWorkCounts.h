#pragma once

#include <cstdint>

namespace Luminumbra::Persistence {

// Observational work only: never serialized, hashed, or used to select save inputs.
// Counts attempts, including work discarded by a failed save. Rewrites count published
// region payloads (including unchanged lod-0 siblings), even if a later member fails.
struct SaveWorkCounts {
    std::uint64_t world_decodes = 0;
    std::uint64_t chunk_decodes = 0;
    std::uint64_t owners_serialized = 0;
    std::uint64_t clean_siblings_serialized = 0;
    std::uint64_t owners_rewritten = 0;
    std::uint64_t regions_rewritten = 0;
    std::uint64_t bytes_compressed = 0; // Uncompressed bytes submitted to LZ4.

    bool operator==(const SaveWorkCounts&) const = default;

    SaveWorkCounts operator-(const SaveWorkCounts& previous) const {
        return {.world_decodes = world_decodes - previous.world_decodes,
                .chunk_decodes = chunk_decodes - previous.chunk_decodes,
                .owners_serialized = owners_serialized - previous.owners_serialized,
                .clean_siblings_serialized =
                    clean_siblings_serialized - previous.clean_siblings_serialized,
                .owners_rewritten = owners_rewritten - previous.owners_rewritten,
                .regions_rewritten = regions_rewritten - previous.regions_rewritten,
                .bytes_compressed = bytes_compressed - previous.bytes_compressed};
    }
};

} // namespace Luminumbra::Persistence
