#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>

namespace Luminumbra::Rendering {

enum class DiagnosticOwnership : std::uint8_t {
    Live = 1,
    LegacyFar = 2,
    LadderFar = 3
};
enum class DiagnosticPass : std::uint8_t {
    GBuffer = 1,
    Forward = 2,
    Shadow = 3,
    Sky = 4
};

struct DiagnosticIdentity {
    std::uint32_t draw = 0;
    std::uint32_t mesh = 0;
    std::uint32_t lod = 0;
    DiagnosticOwnership ownership = DiagnosticOwnership::Live;
    DiagnosticPass pass = DiagnosticPass::GBuffer;
};

// RGBA32UI: draw, capture-local mesh-table index, LOD, ownership | (pass << 8).
// All-zero is background. Nonzero pixels require draw/mesh > 0, LOD <= 255,
// known ownership/pass and zero reserved bits. No native struct is serialized.
using DiagnosticId = std::array<std::uint32_t, 4>;
bool EncodeDiagnosticId(const DiagnosticIdentity& identity, DiagnosticId& output);
bool ValidDiagnosticId(const DiagnosticId& id);

struct DiagnosticExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// At most 16,777,216 pixels, with either dimension <= 16,384. Writers allocate
// no image storage; their fixed scratch buffer is 4 KiB. Input rows are top-first.
enum class DiagnosticAov {
    Albedo,
    Normal,
    Depth,
    Material
};

// Pure readback codecs: no GL, environment lookup or filesystem side effects.
// Invalid dimensions, exact-size mismatch or invalid IDs/materials write nothing.
// Use binary streams. An I/O failure can leave partial bytes; the caller owns
// unpublished output, close/flush checks and publication of bundle metadata last.
bool WriteDiagnosticIds(std::ostream& stream,
                        const DiagnosticExtent& extent,
                        std::span<const DiagnosticId> pixels);
bool WriteDiagnosticAov(std::ostream& stream,
                        const DiagnosticExtent& extent,
                        DiagnosticAov aov,
                        std::span<const float> samples);

struct DiagnosticSample {
    DiagnosticId id{};
    std::array<float, 3> albedo{}; // linear, unlit RGB
    std::array<float, 3> normal{}; // decoded view-space normal, signed XYZ
    float depth = 0;               // raw hardware depth, including reversed-Z
    std::uint32_t material = 0;    // exact G-buffer material ID (0..255)
};

// Schema 1, top-left integer coordinates. Nonfinite channels become JSON null,
// with non_finite=true. Invalid coordinates/IDs/material return an empty string.
// The caller samples every channel from the same completed frame and pixel.
std::string BuildDiagnosticProbe(const DiagnosticExtent& extent,
                                 std::uint32_t x,
                                 std::uint32_t y,
                                 const DiagnosticSample& sample);

} // namespace Luminumbra::Rendering
