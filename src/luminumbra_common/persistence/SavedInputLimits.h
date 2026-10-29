#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Luminumbra::Persistence::SavedInputLimits {
// Named compatibility limits for issue #584 and issue #586. Unbounded historical
// writers now refuse rather than truncate. Bytes always mean encoded file bytes.
// Policy decision: metadata extensions and manifests may occupy at most 2 MiB.
inline constexpr std::size_t kMetadataBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kManifestBytes = 2u * 1024u * 1024u;
// Embedded presets share the 2 MiB document budget.
inline constexpr std::size_t kPresetBytes = 2u * 1024u * 1024u;
// Count object/array containers, including the root. Check text before parsing
// and component values before recursive copying or serialization.
inline constexpr std::size_t kJsonNestingDepth = 64u;

inline bool JsonDepthAllowed(std::string_view bytes) {
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    // One pass with constant storage; callers bound the encoded input bytes.
    for (const char ch : bytes) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '{' || ch == '[') {
            if (depth == kJsonNestingDepth)
                return false;
            ++depth;
        } else if ((ch == '}' || ch == ']') && depth > 0) {
            --depth;
        }
    }
    // Syntax validation belongs to the parser, after depth admission.
    return true;
}
// Policy decision for previously unbounded UTF-8 world name and seed text.
inline constexpr std::size_t kWorldNameBytes = 4u * 1024u;
inline constexpr std::size_t kWorldSeedBytes = 4u * 1024u;
// Policy decision: whole plant JSON, including all extension data, is 256 MiB.
inline constexpr std::size_t kPlantFileBytes = 256u * 1024u * 1024u;
// Policy decision: a component's compact JSON is 1 MiB; their compact JSON sum
// is 128 MiB. The whole-file limit additionally covers names, types and whitespace.
inline constexpr std::size_t kPlantComponentBytes = 1024u * 1024u;
inline constexpr std::size_t kPlantComponentAggregateBytes = 128u * 1024u * 1024u;
// Existing decompression admission is 64 MiB; apply it to writers too. LZ4's
// documented compressBound is n + n/255 + 16, so incompressible output also reloads.
inline constexpr std::size_t kRegionRecordBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kRegionCompressedBytes =
    kRegionRecordBytes + kRegionRecordBytes / 255u + 16u;
// The raw upsert writer permits 65535 records (u16 count), including arbitrary
// chunk Y coordinates; 32*32 is not a legitimate record maximum. Twice its
// largest encoded region is the file bound; use u64 to avoid 32-bit overflow.
inline constexpr std::size_t kRegionRecords = 65535u;
inline constexpr std::uint64_t kRegionFileBytes =
    2ull * (8ull + kRegionRecords * (18ull + kRegionCompressedBytes));
// Length admission stays in u64 even on 32-bit hosts; the absolute format
// ceiling is intentionally looser than the aggregate-work budget below.
inline constexpr bool RegionFileLengthAllowed(std::uint64_t length) {
    return length <= kRegionFileBytes;
}
// Policy decision for previously unbounded aggregate decompression: writers
// permit 512 MiB per region; readers admit twice that maximum before any payload.
inline constexpr std::uint64_t kRegionWriteWorkBytes = 512ull * 1024u * 1024u;
inline constexpr std::uint64_t kRegionReadWorkBytes = 2u * kRegionWriteWorkBytes;
// Twice the maximum writer compressed work: sum(compressBound(n)) is at most
// sum(n) + sum(n)/255 + 16*record_count. Checked before buffering payloads.
inline constexpr std::uint64_t kRegionCompressedAggregateBytes =
    2u * (kRegionWriteWorkBytes + kRegionWriteWorkBytes / 255u + 16u * kRegionRecords);
} // namespace Luminumbra::Persistence::SavedInputLimits
