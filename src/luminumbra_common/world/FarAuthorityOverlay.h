#pragma once

// Far authority overlay (FSV1): the per-tile file that carries edit-baked
// signed-distance bricks for the volumetric far ladder. Byte layout, refusal
// table and limits are documented in docs/far-authority-overlay.md. The overlay
// is derived render state: it never enters a world hash and older builds ignore
// the `far/` directory.

#include "FarTierTable.h"
#include "FarWantedSet.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::World {

inline constexpr std::uint16_t kFsv1Version = 1;
inline constexpr std::size_t kFsv1HeaderBytes = 44;
inline constexpr std::size_t kFsv1TrailerBytes = 4;
inline constexpr std::size_t kFsv1SamplesPerBrick = 125; // 5 x 5 x 5
inline constexpr std::uint32_t kFsv1MaxBricks = 1u << 17;
inline constexpr std::uint64_t kFsv1MaxFileBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::int32_t kFsv1MaxAbsBrickY = 1 << 20;
// Bricks per tile edge: tile edge / brick edge (128 * spacing / (4 * spacing)).
inline constexpr std::int64_t kFsv1BricksPerTileEdge = 32;
// Tile indices beyond this magnitude cannot be expressed as i32 brick indices.
inline constexpr std::int64_t kFsv1MaxAbsTileIndex = 1ll << 25;
inline constexpr std::int16_t kFsv1ForbiddenDensity = -32768;
inline constexpr float kFsv1DensityScale = 256.0f;

enum class FarBrickState : std::uint8_t {
    Present = 0,
    HomogeneousAir = 1,
    HomogeneousSolid = 2
};

struct FarOverlayBrick {
    std::int32_t bx = 0;
    std::int32_t by = 0;
    std::int32_t bz = 0;
    FarBrickState state = FarBrickState::Present;
    // Only meaningful for Present bricks; homogeneous bricks carry no samples.
    std::array<std::int16_t, kFsv1SamplesPerBrick> density{};
    std::array<std::uint8_t, kFsv1SamplesPerBrick> material{};
};

struct FarOverlayTile {
    FarTileKey key;
    std::uint64_t generation = 0;        // opaque save-level snapshot generation
    std::vector<FarOverlayBrick> bricks; // sorted by (bz, bx, by), no duplicates
};

enum class FarOverlayError : std::uint8_t {
    None,
    NotFound,
    IoError,
    BadMagic,
    Truncated,
    CrcMismatch,
    FutureVersion,
    CorruptVersion, // version zero
    ReservedBits,   // flags or reserved bytes non-zero
    InvalidTier,
    InvalidKey,  // tile index outside kFsv1MaxAbsTileIndex or tier invalid on encode
    KeyMismatch, // header key differs from the expected key
    BrickOutOfFootprint,
    BricksNotSorted, // unsorted or duplicate
    InvalidBrickState,
    ForbiddenDensity,
    LimitExceeded,
    SizeMismatch, // body_bytes disagrees with the records
    TrailingBytes,
};

struct FarOverlayDecodeResult {
    FarOverlayError error = FarOverlayError::None;
    FarOverlayTile tile; // empty unless error == None
};

// Short stable name for diagnostics ("CrcMismatch", ...).
const char* FarOverlayErrorName(FarOverlayError error);

// Validates the tile (same invariants as decode) and serializes it. On any error
// `out` is cleared and the error returned.
FarOverlayError EncodeFarOverlay(const FarOverlayTile& tile, std::string& out);

// Decodes and validates `bytes` as the overlay for `expected_key`. Never returns
// a partial tile.
FarOverlayDecodeResult DecodeFarOverlay(std::string_view bytes, const FarTileKey& expected_key);

// `<save_dir>/far/v<tier>.<tx>.<tz>.lmr` with negative indices printed with '-'.
std::filesystem::path FarOverlayPath(const std::filesystem::path& save_dir, const FarTileKey& key);

// Parses a file name of the form v<tier>.<tx>.<tz>.lmr. Returns nullopt for any
// other name, a tier outside 1..5, a non-canonical number (leading zeros, '+',
// "-0") or a number that overflows i64.
std::optional<FarTileKey> ParseFarOverlayFileName(std::string_view file_name);

// Encodes the tile and writes it with WorldSaveService::write_file_durably. A
// failure leaves any previous file for that key in place.
bool WriteFarOverlay(const std::filesystem::path& save_dir,
                     const FarOverlayTile& tile,
                     std::vector<std::string>* errors = nullptr);

// Reads and decodes the overlay for `key`. A missing file gives NotFound; a file
// larger than kFsv1MaxFileBytes is refused before it is read.
FarOverlayDecodeResult ReadFarOverlay(const std::filesystem::path& save_dir, const FarTileKey& key);

// Decodes every `<save_dir>/far/v*.lmr` (sorted by file name). A missing `far/`
// directory is valid. Returns false and appends one message per refused file when
// any overlay is corrupt, from a newer format, or disagrees with its file name.
// Files whose name does not parse as an overlay name are ignored.
bool ValidateFarOverlayDirectory(const std::filesystem::path& save_dir,
                                 std::vector<std::string>* errors = nullptr);

// Pure bake derivation. No edited chunks: not due. Edited chunks and no overlay:
// due (rebuild from chunk records, never pristine). Otherwise due iff the newest
// edited chunk generation is greater than the overlay generation.
constexpr bool FarBakeDue(std::optional<std::uint64_t> overlay_generation,
                          std::optional<std::uint64_t> max_edited_chunk_generation) {
    if (!max_edited_chunk_generation.has_value()) {
        return false;
    }
    if (!overlay_generation.has_value()) {
        return true;
    }
    return *max_edited_chunk_generation > *overlay_generation;
}

// Quantizes a signed distance in metres (negative solid, positive air) to the
// stored density: round(value * 256) clamped to [-32767, 32767]; a non-zero input
// that rounds to zero becomes +1 or -1 with the sign of the input so sign is
// preserved; NaN becomes 0.
std::int16_t QuantizeFarDensity(float signed_distance_meters);

// Homogeneous detection: all 125 densities < 0 is HomogeneousSolid, all > 0 is
// HomogeneousAir, anything else (including any exact 0) is Present.
FarBrickState ClassifyFarBrick(const std::array<std::int16_t, kFsv1SamplesPerBrick>& density);

} // namespace Luminumbra::World
