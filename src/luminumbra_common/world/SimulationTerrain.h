#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Luminumbra::world {

// Owned simulation data, independent of resident chunks, meshing and LOD. Single-owner access:
// admission must not overlap queries. Missing data is unavailable authority, never procedural air.
class SimulationTerrain {
public:
    static constexpr std::int64_t kChunkCells = 16;
    static constexpr std::size_t kLatticeSide = 17;
    static constexpr std::size_t kLatticeSamples = kLatticeSide * kLatticeSide * kLatticeSide;
    static constexpr std::size_t kMaxLattices = 4096;
    static constexpr std::size_t kMaxOwnedBytes = 96 * 1024 * 1024;
    static constexpr std::size_t kMaxVerticalCells = 65536;

    struct Key {
        std::int32_t x = 0, y = 0, z = 0;
        auto operator<=>(const Key&) const = default;
    };
    struct Limits {
        std::size_t lattices = kMaxLattices;
        std::size_t owned_bytes = kMaxOwnedBytes;
    };
    enum class Admission {
        Accepted,
        Unavailable,
        WorkLimit
    };
    enum class Status {
        Surface,
        NoSurface,
        WorkLimit,
        Unavailable
    };
    // Per-call const diagnostic; no mutable counters or exposed storage. Index comparisons are
    // bounded by the 4096-entry index. Samples counts actual density reads (eight per cell).
    struct Visits {
        std::size_t cells = 0;
        std::size_t lattice_lookups = 0;
        std::size_t samples = 0;
        bool operator==(const Visits&) const = default;
    };
    struct Result {
        Status status = Status::Unavailable;
        std::int64_t y_mm = 0; // meaningful only for Surface
        Visits visits{};
    };

    SimulationTerrain();
    explicit SimulationTerrain(const Limits& limits);
    ~SimulationTerrain();
    SimulationTerrain(const SimulationTerrain&) = delete;
    SimulationTerrain& operator=(const SimulationTerrain&) = delete;

    // Copies exactly 17^3 finite floats in x + 17*y + 17^2*z order. All shared nodes of
    // admitted neighbours (faces, edges, corners) must agree exactly. Invalid/partial input
    // returns Unavailable; limits/allocation failure return WorkLimit. Every refusal preserves
    // existing data, including replacements. No generation or fallback happens here.
    // Storage is at most 4096 retained records, one staged record and 4096 index pointers.
    // owned_bytes caps requested allocation bytes, including the temporary replacement record,
    // excluding allocator metadata.
    // All size checks precede allocation. Admission shifts at most 4096 pointers, never lattices.
    [[nodiscard]] Admission admit(Key key, std::span<const float> samples);

    // First downward air-to-solid crossing within inclusive [min_y_mm, start_y_mm]. Air is
    // positive density, solid is nonpositive. Interpolate in double, X then Z (explicit fma),
    // then solve the linear vertical cell exactly in that interpolant. A zero endpoint counts
    // only when density increases upwards: contact stays in place, a roof underside does not.
    // Starting inside solid skips its upper surface and searches below for another crossing.
    // A constant-zero cell alone is not a crossing. Round the solved world height once to mm,
    // nearest with half ties away from zero. Integer division floors negative coordinates.
    // Cells own [base, base+1]; exact X/Z faces use the positive-side chunk. Exact starting Y
    // uses the cell below, so a contact needs only its supporting lattice. No upward search.
    // Coordinates must map to int32 chunk keys; bad/reversed ranges return Unavailable.
    // The entire requested cell span is checked against min(max_cells, 65536) before work.
    // NoSurface proves absence only in the requested, fully available interval; gaps refuse.
    // Query allocates nothing, reads eight samples/cell and looks up only crossed lattices.
    [[nodiscard]] Result first_surface(std::int64_t x_mm,
                                       std::int64_t z_mm,
                                       std::int64_t start_y_mm,
                                       std::int64_t min_y_mm,
                                       std::size_t max_cells = kMaxVerticalCells) const;
    [[nodiscard]] std::size_t lattice_count() const;
    [[nodiscard]] std::size_t owned_bytes() const;

private:
    struct Record {
        Key key{};
        std::array<float, kLatticeSamples> samples{};
    };
    using Entry = std::unique_ptr<Record>;
    [[nodiscard]] std::size_t lower_bound(Key key) const;
    [[nodiscard]] const Record* find(Key key) const;
    [[nodiscard]] bool agrees(Key key, std::span<const float> samples) const;

    Limits m_limits;
    // Allocated lazily, once, with exactly limits.lattices pointers; no vector growth slack.
    std::unique_ptr<Entry[]> m_index;
    std::size_t m_size = 0;
};

} // namespace Luminumbra::world
