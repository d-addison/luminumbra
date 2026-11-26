#pragma once

// Synthetic measurement support, shared by the optional tool and fast regressions.
// Never included by runtime targets. Bounds apply before constructing any fixture.
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Luminumbra::RegionMeasurement {

inline constexpr std::uint32_t kMaxLedgerRecords = 1u << 19;
inline constexpr std::uint32_t kMaxPayloadChunks = 4096;
inline constexpr std::size_t kSdfValues = 17u * 17u * 17u;
inline constexpr std::array<const char*, 6> kSchedulerPhases{
    "cold", "warm", "anchor_change", "reduce", "freeze", "idle_evict"};
inline constexpr std::array<const char*, 3> kSavePhases{"first", "unchanged", "sparse_edit"};

inline std::uint32_t ParseSize(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("empty fixture size");
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument("invalid unsigned fixture size");
    }
    return value;
}

inline void ValidateSizes(std::uint32_t records, std::uint32_t chunks) {
    if (records == 0 || records > kMaxLedgerRecords || chunks == 0 || chunks > kMaxPayloadChunks) {
        throw std::invalid_argument("measurement sizes outside fixture bounds");
    }
}

inline world::RegionKey LedgerKey(std::uint32_t index) {
    return {static_cast<std::int32_t>(index % 256) + 10,
            static_cast<std::int32_t>(index / 256) + 10};
}

inline world::ActiveRegionLedger MakeLedger(std::uint32_t count) {
    ValidateSizes(count, 1);
    // Finite synthetic configuration only; no shipping configuration is changed.
    world::ActiveRegionLedger ledger({0, 1, 1});
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto key = LedgerKey(index);
        ledger.activate_owned(key, world::WorldClock{});
        ledger.set_populated(key, index % 2 == 0);
    }
    return ledger;
}

inline std::uint64_t DurableDigest(world::RegionKey key) {
    return (std::uint64_t{static_cast<std::uint32_t>(key.x)} << 32) |
           static_cast<std::uint32_t>(key.z);
}

inline const world::RegionSchedule& SchedulePhase(world::ActiveRegionLedger& ledger,
                                                  std::size_t phase_index) {
    const std::array anchors{Vec3{5376, 0, 5376}};
    const std::array work{world::RegionWork{ledger.records().begin()->first, 1, 0, 0, 0}};
    const auto tick = phase_index == 5 ? world::ActiveRegionLedger::kIdleEvictionTicks + 6
                                       : static_cast<std::uint64_t>(phase_index) + 1;
    return ledger.schedule(
        world::WorldClock(tick),
        phase_index == 2 ? std::span<const Vec3>(anchors) : std::span<const Vec3>{},
        phase_index == 3 || phase_index == 4 ? std::span<const world::RegionWork>(work)
                                             : std::span<const world::RegionWork>{},
        DurableDigest,
        [](world::RegionKey) { return false; });
}

struct LedgerObservation {
    std::uint64_t records = 0, populated_records = 0, empty_unowned_records = 0;
    std::uint64_t encoded_bytes = 0, record_value_bytes = 0, priority_comparisons = 0;
    std::uint64_t due = 0, transitions = 0, evicted = 0;
    double elapsed_ms = 0;
    std::string digest;
};

inline LedgerObservation MeasurePhase(world::ActiveRegionLedger& ledger, std::size_t index) {
    if (index >= kSchedulerPhases.size() || ledger.records().empty()) {
        throw std::invalid_argument("invalid scheduler measurement phase");
    }
    const auto comparisons = ledger.priority_comparisons();
    const auto start = std::chrono::steady_clock::now();
    const auto& schedule = SchedulePhase(ledger, index);
    const auto end = std::chrono::steady_clock::now();
    LedgerObservation result;
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.records = ledger.records().size();
    for (const auto& [key, record] : ledger.records()) {
        (void)key;
        result.populated_records += record.populated ? 1u : 0u;
    }
    result.empty_unowned_records = result.records - result.populated_records;
    result.encoded_bytes = ledger.encode().size();
    result.record_value_bytes = result.records * sizeof(world::ActiveRegionRecord);
    result.priority_comparisons = ledger.priority_comparisons() - comparisons;
    result.due = schedule.due.size();
    result.transitions = schedule.transitions.size();
    result.evicted = schedule.evicted.size();
    result.digest = schedule.digest;
    return result;
}

inline std::vector<std::shared_ptr<Chunk>>
PopulateSave(world::GameSession& session, std::uint32_t count, std::uint32_t records = 1) {
    ValidateSizes(records, count);
    auto* world_system = session.GetWorldSystem();
    if (world_system == nullptr) {
        throw std::invalid_argument("measurement requires an open world");
    }
    for (std::uint32_t index = 0; index < records; ++index) {
        session.PinActiveRegion(LedgerKey(index), true);
    }
    std::vector<std::shared_ptr<Chunk>> chunks;
    chunks.reserve(count);
    // Pairs share one persistence region; each pair has a distinct region. The
    // cap bounds SDF storage to 4096 * 4913 floats, plus three height samples.
    for (std::uint32_t index = 0; index < count; ++index) {
        const IVec3 coords{
            static_cast<std::int32_t>(index / 2) * 32, static_cast<std::int32_t>(index % 2), 0};
        auto chunk = std::make_shared<Chunk>(coords);
        chunk->set_state(ChunkState::Meshing);
        chunk->sdf_data.assign(kSdfValues, -4.0f);
        chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
        chunk->mark_voxel_data_dirty();
        if (!world_system->adopt_streamed_chunk(chunk)) {
            throw std::runtime_error("measurement chunk admission refused");
        }
        int region_x = 0, region_z = 0;
        Persistence::WorldSaveService::region_coords_for_chunk(coords, region_x, region_z);
        session.PinActiveRegion({region_x, region_z}, true);
        chunks.push_back(chunk);
    }
    return chunks;
}

struct SaveObservation {
    Persistence::SaveWorkCounts work{};
    std::uint64_t snapshot_copies = 0, chunks = 0, payload_value_bytes = 0;
    std::uint64_t ledger_records = 0, ledger_encoded_bytes = 0;
    double elapsed_ms = 0;
};

inline SaveObservation MeasureSave(world::GameSession& session, std::uint32_t count) {
    ValidateSizes(1, count);
    if (session.GetWorldSystem() == nullptr) {
        throw std::invalid_argument("measurement requires an open world");
    }
    const auto copies = session.GetWorldSystem()->snapshot_copies();
    world::WorldStateSaveReport report;
    const auto start = std::chrono::steady_clock::now();
    const auto saved = session.SaveWorldState(&report);
    const auto end = std::chrono::steady_clock::now();
    if (!saved) {
        throw std::runtime_error("indexed measurement save refused");
    }
    if (!Persistence::snapshot_index_exists(session.GetWorldSaveDir())) {
        throw std::runtime_error("measurement save did not publish an index");
    }
    return {.work = report.work,
            .snapshot_copies = session.GetWorldSystem()->snapshot_copies() - copies,
            .chunks = report.chunks_total,
            .payload_value_bytes = std::uint64_t{count} * (kSdfValues + 3) * sizeof(float),
            .ledger_records = session.GetActiveRegionLedger().records().size(),
            .ledger_encoded_bytes = session.GetActiveRegionLedger().encode().size(),
            .elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count()};
}

} // namespace Luminumbra::RegionMeasurement
