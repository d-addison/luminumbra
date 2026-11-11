#pragma once

#include "../core/WaterComponents.h"
#include "../world/WaterShare.h"
#include "WaterSystem.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Luminumbra::Systems::RegionalWater {

// Bounds apply before copying or sorting inputs. Planning uses only the supplied
// non-loading participation snapshot, never a world-store scan or durable lookup.
inline constexpr std::size_t kMaxResidents = std::size_t{1} << 20;
inline constexpr std::size_t kMaxSources = std::size_t{1} << 20;
struct Resident {
    ChunkID id = 0;
    bool initialized = true;
    int resolution = 8;
    bool awake = true;
    bool preparation_ready = true;
    bool available = true; // False for loading or otherwise unavailable residents.
};
struct SourceInput {
    std::uint64_t id = 0; // Unique snapshot identity, independent of input order.
    Vec3 position{0.0f};
    Components::FlowingWaterSourceComponent physical{};
};
struct Inputs {
    int resolution = 8;
    std::uint64_t cell_budget = 0;
    bool raining = false;
};
struct Source {
    ChunkID target = 0;
    std::size_t cell = 0;
    std::int32_t millimetres = 0;
    auto operator<=>(const Source&) const = default;
};
struct Face {
    world::RegionKey owner{}, neighbor{};
    ChunkID positive = 0, other = 0;
    bool is_x = false;
    std::uint8_t common_shift = 0; // Coarser owner cadence; eligibility uses both persisted phases.
    auto operator<=>(const Face&) const = default;
};
struct RegionPlan {
    world::RegionKey owner{};
    std::uint8_t cadence_shift = 0;
    std::uint32_t cadence_phase = 0;
    std::uint64_t candidates = 0, windows = 0, next_cursor = 0, debt = 0;
    bool operator==(const RegionPlan&) const = default;
};
struct Plan {
    // IDs, sources, faces and owner outputs have canonical ascending order.
    std::vector<ChunkID> interiors, participants;
    std::vector<Source> sources; // Admitted snapshot; apply only to interior targets.
    std::vector<Face> faces;
    std::vector<RegionPlan> regions;
    std::vector<world::RegionKey> refused_preparation;
    std::uint64_t global_windows = 0, border_windows = 0, allocated_windows = 0;
    std::uint64_t preparation_cells = 0, total_cells = 0;
    std::size_t residents_visited = 0; // Snapshot admission visits; no ledger traversal.
    bool operator==(const Plan&) const = default;
};

// Pure planning: no chunk, ledger, cursor, debt or revision is changed. Invalid
// inputs throw invalid_argument before budget allocation. Unready preparation seals the
// whole owner without spending windows or crediting debt. Unknown/frozen owners
// are excluded. Interiors and both owners of a face require due membership and
// their persisted cadence phase.
// Each window pays for preparation, one interior and four face endpoints. Keep
// one window for interior credit when claims exist, then reserve cross-owner pairs
// atomically. Fractional owner credit can defer interior selection to a later call.
// Interior credit is capped at serviceable demand: the total admitted due candidate
// count, with one interior window usable per candidate. Existing debt is retained;
// excess capacity creates no new debt. Post-credit debt must still fit kMaxDebt.
// After owner credit allocation, an owner granted at least two interior windows
// spends two of them on an eligible same-owner pair on odd owner cadence
// opportunities (tick >> cadence_shift). Rotate its sorted eligible pairs by
// (opportunity/2) modulo its pair count F. With fixed eligibility and at least two
// awarded windows per due call, every pair is served within 2*F owner cadence
// opportunities; this includes a sole owner with two remaining windows. Even
// opportunities retain ordinary cursor service. Pair selection never changes
// owner credit or grants, preserving the allocator's owner service guarantee.
// Zero-candidate owners never compete for allocation; their debt and cursor are
// carried unchanged.
// Sorted eligible cross-owner faces start at (tick >> coarsest face cadence)
// modulo their count. With a fixed eligible set and at least one funded pair,
// all N faces are served within N common cadence opportunities, including skipped
// off-phase ticks. Face outputs are sorted again after reservation.
// Region outputs are patches for due admitted owners only (debt zero means erase);
// all other registers must be retained by the eventual publisher. Region windows
// and allocated_windows count selected interiors only. Unselected grants retain
// their Q32 debt; unused windows are global_windows-border_windows-allocated_windows.
// Cursor rollover serves every candidate under fixed eligibility and recurring
// positive awards on even owner cadence opportunities. Changing eligibility or
// funding only pair opportunities has no such cursor guarantee.
// Work: O((R+S+D) log(kMaxRegions+R+S+D)). Storage is bounded by the input
// caps: at most R entries, S sources, D region outputs, 2*R topology faces,
// 2*R pair pointers, 2*R output faces (including eligible borders before budget
// truncation) and 5*R participant IDs before deduplication. All products fit 32-bit size_t.
// Callers supply participating snapshots; this API has no production tick caller.
[[nodiscard]] Plan Build(std::span<const Resident> residents,
                         std::span<const SourceInput> sources,
                         const world::ActiveRegionLedger& ledger,
                         const world::RegionSchedule& schedule,
                         const Inputs& inputs);

// One owner-thread transaction per executor, with no live tick caller yet.
// Begin captures only the supplied participants (<= kMaxResidents, <= 1024 cells
// each) and owner patches (<= kMaxRegions). Hold exclusive mutation access to
// those chunks and the ledger through Publish/Rollback; mesh workers use copies.
// Kernel failure/exception discards all pending work. Publication validates every
// stage before the first live write, then commits without allocating or refusing.
// Accepted ticks are consumed even by Rollback; repeated/backward Begin refuses.
// Begin costs O(P log P + P*1024 + D log N); Publish costs O(P*1024 + D log N).
// Storage is O(P*1024 + D), never O(world).
class Transaction {
public:
    Transaction() = default;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    void Begin(world::ActiveRegionLedger& ledger,
               std::uint64_t tick,
               std::span<const std::shared_ptr<Chunk>> participants,
               std::span<const RegionPlan> regions);
    WaterKernels::Result StepInterior(ChunkID id,
                                      SHIELD_WorldSystem& shield,
                                      const WaterKernels::InteriorInputs& inputs);
    WaterKernels::Result StepFace(ChunkID positive, ChunkID other, bool is_x);
    void Publish();
    void Rollback() noexcept;
    [[nodiscard]] bool pending() const noexcept {
        return m_ledger != nullptr;
    }
    // Const diagnostics of private staged storage and actual capture visits.
    [[nodiscard]] const Chunk* staged(ChunkID id) const noexcept;
    [[nodiscard]] std::size_t participants_visited() const noexcept {
        return m_visited;
    }

private:
    struct Entry {
        std::shared_ptr<Chunk> live, grid;
        std::uint64_t authority_revision = 0;
        std::uint32_t water_revision = 0;
        bool changed = false;
    };
    Chunk& Grid(ChunkID id);
    void Account(const WaterKernels::Result& result, const Chunk& grid);
    std::vector<Entry> m_entries;
    std::vector<world::ActiveRegionLedger::WaterPatch> m_patches;
    world::ActiveRegionLedger::WaterDebts m_debts;
    world::ActiveRegionLedger* m_ledger = nullptr;
    std::optional<std::uint64_t> m_lastTick;
    std::int64_t m_expectedVolume = 0;
    std::size_t m_visited = 0;
};

} // namespace Luminumbra::Systems::RegionalWater
