#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra_common/world/ActivationResidencyIndex.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>

namespace {
using namespace Luminumbra;
using Systems::SHIELD_WorldSystem;
using Index = World::ActivationResidencyIndex;
using Candidate = Index::Candidate;

// Independent transcription of the full-scan priority contract: do not use the
// index comparator here, so ordering regressions cannot change both sides.
auto PriorityKey(const Candidate& candidate) {
    return std::tuple(!candidate.interior_focus,
                      !candidate.surface,
                      candidate.ring_distance,
                      candidate.horizontal_distance_sq,
                      candidate.vertical_rank,
                      candidate.coords.y,
                      candidate.coords.x,
                      candidate.coords.z);
}

void ExpectPending(const Index& index,
                   const std::vector<Candidate>& wanted,
                   const std::set<ChunkID>& resident) {
    std::vector<Candidate> expected;
    for (const auto& candidate : wanted) {
        if (!resident.contains(Chunk::calculate_id(candidate.coords)))
            expected.push_back(candidate);
    }
    std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
        return PriorityKey(a) < PriorityKey(b);
    });
    ASSERT_EQ(index.pending().size(), expected.size());
    auto actual = index.pending().begin();
    std::size_t surface_count = 0;
    for (const auto& candidate : expected) {
        EXPECT_EQ(actual->coords, candidate.coords);
        EXPECT_EQ(PriorityKey(*actual), PriorityKey(candidate));
        surface_count += candidate.surface ? 1u : 0u;
        ++actual;
    }
    EXPECT_EQ(index.surface_pending(), surface_count);
}

TEST(ActivationResidencyIndex, ResidencyDeltasPreserveFullScanOrder) {
    Index index;
    std::mt19937 random(0x401b7u);
    std::map<ChunkID, std::shared_ptr<Chunk>> chunks;
    std::set<ChunkID> resident;
    std::vector<Candidate> wanted;
    // Fixture bound: 147 candidates, 400 transitions; no world jobs or timing.
    for (int z = -3; z <= 3; ++z) {
        for (int x = -3; x <= 3; ++x) {
            for (int y = -1; y <= 1; ++y) {
                Candidate candidate{.coords = IVec3(x, y, z),
                                    .surface = y == 0,
                                    .ring_distance = std::max(std::abs(x), std::abs(z)),
                                    .horizontal_distance_sq = x * x + z * z,
                                    .vertical_rank = std::abs(y),
                                    .interior_focus = random() % 7u == 0u};
                wanted.push_back(candidate);
                chunks.emplace(Chunk::calculate_id(candidate.coords),
                               std::make_shared<Chunk>(candidate.coords));
            }
        }
    }
    index.replace_wanted(wanted);
    for (unsigned iteration = 0; iteration < 400; ++iteration) {
        const auto& candidate = wanted[random() % wanted.size()];
        const auto id = Chunk::calculate_id(candidate.coords);
        if (resident.erase(id) != 0) {
            index.erase(*chunks.at(id));
        } else {
            resident.insert(id);
            index.insert(chunks.at(id));
            // Replacement at an existing id must not decrement counts twice.
            index.insert(chunks.at(id));
        }
        if (iteration % 11u == 0u) {
            index.content_changed(candidate.coords);
            EXPECT_TRUE(index.dirty_columns().contains(
                Index::column_key(candidate.coords.x, candidate.coords.z)));
        }
        if (iteration % 31u == 0u) {
            for (auto& entry : wanted) {
                entry.ring_distance = static_cast<int>(random() % 5u);
                entry.horizontal_distance_sq = static_cast<int>(random() % 25u);
            }
            index.replace_wanted(wanted);
        }
        ExpectPending(index, wanted, resident);
        index.clear_dirty_columns();
    }
    for (const auto id : resident)
        index.erase(*chunks.at(id));
    EXPECT_TRUE(index.resident_columns().empty());
    ExpectPending(index, wanted, {});
    index.clear();
    EXPECT_TRUE(index.pending().empty());
    EXPECT_TRUE(index.columns().empty());
    EXPECT_TRUE(index.dirty_columns().empty());
}

TEST(ActivationResidencyIndex, EveryContentVersionInvalidatesItsSpan) {
    Index index;
    int samples = 0;
    int height = 2;
    auto sampler = [&](int, int) {
        ++samples;
        return Index::SurfaceSpan{
            .min_y = height, .max_y = height + 2, .center_y = height + 1, .biome_id = 7};
    };
    EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, 2);
    EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, 2);
    EXPECT_EQ(samples, 1);
    for (int version = 1; version <= 40; ++version) {
        height += 3;
        index.content_changed(IVec3(-32, version % 3, 31));
        EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, height);
        EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, height);
        const auto& column = index.columns().at(Index::column_key(-32, 31));
        EXPECT_EQ(column.content_version, static_cast<std::uint64_t>(version));
        EXPECT_EQ(column.span_version, column.content_version);
    }
    EXPECT_EQ(samples, 41);
    // Another column's content never invalidates this one.
    index.content_changed(IVec3(32, 0, -31));
    EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, height);
    EXPECT_EQ(samples, 41);
    index.invalidate_spans();
    EXPECT_EQ(index.surface_span(-32, 31, sampler).min_y, height);
    EXPECT_EQ(samples, 42);
    EXPECT_EQ(index.counters().span_cache_misses, 42u);
    EXPECT_EQ(index.counters().span_cache_hits, 42u);
}

TEST(ActivationResidencyIndex, TravelReclaimsColumnsAfterResidencyAndWantedRemoval) {
    Index index;
    const auto sampler = [](int, int) {
        return Index::SurfaceSpan{};
    };
    // Fixture bound: 128 columns, two residents and two wanted entries per stop.
    // Exercise both removal orders, including a cache-only sampled column.
    for (int x = 0; x < 128; ++x) {
        SCOPED_TRACE(x);
        const auto lower = std::make_shared<Chunk>(IVec3(x, 0, 0));
        const auto upper = std::make_shared<Chunk>(IVec3(x, 1, 0));
        const std::vector<Candidate> wanted{
            Candidate{.coords = lower->get_coords(), .surface = true},
            Candidate{.coords = upper->get_coords(), .surface = true}};
        (void)index.surface_span(x, 0, sampler);
        (void)index.surface_span(x, 1, sampler);
        index.replace_wanted(wanted);
        ASSERT_EQ(index.columns().size(), 1u);
        index.insert(lower);
        index.insert(upper);
        index.erase(*lower);
        EXPECT_EQ(index.columns().size(), 1u);
        EXPECT_EQ(index.resident_columns().size(), 1u);
        ExpectPending(index, wanted, {upper->get_id()});
        if (x % 2 == 0) {
            index.erase(*upper);
            EXPECT_EQ(index.columns().size(), 1u);
            EXPECT_TRUE(index.resident_columns().empty());
            ExpectPending(index, wanted, {});
            index.replace_wanted({});
        } else {
            index.replace_wanted({});
            EXPECT_EQ(index.columns().size(), 1u);
            index.erase(*upper);
        }
        EXPECT_TRUE(index.columns().empty());
        EXPECT_TRUE(index.resident_columns().empty());
        EXPECT_TRUE(index.pending().empty());
        EXPECT_EQ(index.surface_pending(), 0u);
        // A repeated removal must not recreate a column or stale cache key.
        index.erase(*upper);
        index.replace_wanted({});
        EXPECT_TRUE(index.columns().empty());
        index.clear_dirty_columns();
    }
    // Reset/replacement must forget both resident and cache-only bookkeeping.
    const auto adopted = std::make_shared<Chunk>(IVec3(-1, 0, 0));
    index.insert(adopted);
    index.content_changed(adopted->get_coords());
    (void)index.surface_span(-2, 0, sampler);
    index.replace_wanted({Candidate{.coords = IVec3(-2, 0, 0)}});
    index.clear();
    EXPECT_TRUE(index.columns().empty());
    EXPECT_TRUE(index.resident_columns().empty());
    EXPECT_TRUE(index.pending().empty());
    EXPECT_TRUE(index.dirty_columns().empty());
    index.insert(adopted);
    index.replace_wanted({});
    ASSERT_EQ(index.columns().size(), 1u);
    index.erase(*adopted);
    EXPECT_TRUE(index.columns().empty());
}

Index::SurfaceSpan FullScanSpan(const SHIELD_WorldSystem& world, int x, int z) {
    const float base_x = static_cast<float>(x * CHUNK_SIZE_X);
    const float base_z = static_cast<float>(z * CHUNK_SIZE_Z);
    const std::array<Vec3, 5> positions{
        Vec3(base_x + CHUNK_SIZE_X * 0.5f, 0, base_z + CHUNK_SIZE_Z * 0.5f),
        Vec3(base_x, 0, base_z),
        Vec3(base_x + CHUNK_SIZE_X, 0, base_z),
        Vec3(base_x, 0, base_z + CHUNK_SIZE_Z),
        Vec3(base_x + CHUNK_SIZE_X, 0, base_z + CHUNK_SIZE_Z)};
    std::array<float, 5> xs{};
    std::array<float, 5> zs{};
    std::array<float, 5> heights{};
    for (std::size_t sample = 0; sample < positions.size(); ++sample) {
        xs[sample] = positions[sample].x;
        zs[sample] = positions[sample].z;
    }
    // The public position sampler always applies complete shaping. Legacy
    // activation omits four optional features, so build an independent sampler
    // with those features disabled instead of consulting the production span cache.
    // Only the legacy shaped fixture needs this fallback; each call samples five points.
    const auto& params = world.get_params();
    std::unique_ptr<SHIELD_WorldSystem> legacy_sampler;
    if (params.surface_sampling_revision < Systems::kSurfaceSamplingRevision &&
        (params.biome_relief_enabled || params.cliffs_enabled || params.lakes_enabled ||
         params.surface_breaks_enabled)) {
        auto legacy_params = params;
        legacy_params.biome_relief_enabled = false;
        legacy_params.cliffs_enabled = false;
        legacy_params.lakes_enabled = false;
        legacy_params.surface_breaks_enabled = false;
        legacy_sampler =
            std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, legacy_params, world.get_seed());
    }
    const auto& sampler = legacy_sampler ? *legacy_sampler : world;
    sampler.ComputeShapedHeightsAtPositions(xs.data(), zs.data(), heights.size(), heights.data());
    Index::SurfaceSpan span;
    for (std::size_t sample = 0; sample < positions.size(); ++sample) {
        const int y =
            static_cast<int>(std::floor(heights[sample] / static_cast<float>(CHUNK_SIZE_Y)));
        if (sample == 0) {
            span.min_y = y;
            span.max_y = y;
            span.center_y = y;
        } else {
            span.min_y = std::min(span.min_y, y);
            span.max_y = std::max(span.max_y, y);
        }
    }
    return span;
}

// The full-scan oracle has no access to the production cache or residency index.
std::vector<Candidate>
FullScan(const SHIELD_WorldSystem& world, const std::vector<Vec3>& anchors, int radius) {
    std::map<ChunkID, Candidate> candidates;
    const auto add = [&](const IVec3& coords, bool surface, int dx, int dz, int rank, bool cave) {
        const Candidate candidate{.coords = coords,
                                  .surface = surface,
                                  .ring_distance = std::max(std::abs(dx), std::abs(dz)),
                                  .horizontal_distance_sq = dx * dx + dz * dz,
                                  .vertical_rank = rank,
                                  .interior_focus = cave};
        const auto [it, inserted] = candidates.emplace(Chunk::calculate_id(coords), candidate);
        if (!inserted) {
            it->second.ring_distance = std::min(it->second.ring_distance, candidate.ring_distance);
            it->second.horizontal_distance_sq =
                std::min(it->second.horizontal_distance_sq, candidate.horizontal_distance_sq);
            it->second.vertical_rank = std::min(it->second.vertical_rank, rank);
            it->second.interior_focus = it->second.interior_focus || cave;
        }
    };
    for (const auto& anchor : anchors) {
        const IVec3 center(static_cast<int>(std::floor(anchor.x / CHUNK_SIZE_X)),
                           static_cast<int>(std::floor(anchor.y / CHUNK_SIZE_Y)),
                           static_cast<int>(std::floor(anchor.z / CHUNK_SIZE_Z)));
        if (center.y < FullScanSpan(world, center.x, center.z).min_y - 1) {
            const int cave_radius = std::min(radius, 8);
            const int cave_height = std::min(cave_radius, 4);
            for (int dz = -cave_radius; dz <= cave_radius; ++dz)
                for (int dx = -cave_radius; dx <= cave_radius; ++dx)
                    for (int dy = -cave_height; dy <= cave_height; ++dy)
                        add(center + IVec3(dx, dy, dz), false, dx, dz, std::abs(dy), true);
        }
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dz * dz > radius * radius)
                    continue;
                const auto span = FullScanSpan(world, center.x + dx, center.z + dz);
                for (int y = span.min_y; y <= span.max_y; ++y)
                    add(IVec3(center.x + dx, y, center.z + dz),
                        true,
                        dx,
                        dz,
                        std::abs(y - span.center_y),
                        false);
                const int ring = std::max(std::abs(dx), std::abs(dz));
                if (ring <= 12) {
                    const int rank = ring <= 4 ? 1 : 2;
                    add(IVec3(center.x + dx, span.min_y - 1, center.z + dz),
                        false,
                        dx,
                        dz,
                        rank,
                        false);
                    add(IVec3(center.x + dx, span.max_y + 1, center.z + dz),
                        false,
                        dx,
                        dz,
                        rank,
                        false);
                }
            }
        }
    }
    std::vector<Candidate> result;
    for (const auto& [id, candidate] : candidates) {
        (void)id;
        if (!world.find_streamed_chunk(candidate.coords))
            result.push_back(candidate);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return PriorityKey(a) < PriorityKey(b);
    });
    return result;
}

class ActivationFullScanParity : public testing::TestWithParam<unsigned> {};

bool FullScanRetains(const SHIELD_WorldSystem& world,
                     const IVec3& coords,
                     const std::vector<Vec3>& anchors,
                     int radius) {
    bool horizontal = false;
    for (const auto& anchor : anchors) {
        const IVec3 center(static_cast<int>(std::floor(anchor.x / CHUNK_SIZE_X)),
                           static_cast<int>(std::floor(anchor.y / CHUNK_SIZE_Y)),
                           static_cast<int>(std::floor(anchor.z / CHUNK_SIZE_Z)));
        const auto delta = coords - center;
        if (std::abs(delta.x) > radius + 2 || std::abs(delta.z) > radius + 2)
            continue;
        horizontal = true;
        if (delta.y <= RENDER_DISTANCE_UP + 2 && delta.y >= -RENDER_DISTANCE_DOWN - 2)
            return true;
    }
    if (!horizontal)
        return false;
    const auto span = FullScanSpan(world, coords.x, coords.z);
    return coords.y >= span.min_y - 1 && coords.y <= span.max_y + 1;
}

TEST_P(ActivationFullScanParity, RandomEditsAnchorMotionAndRegionCrossings) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    params.base_amplitude = 40.0f;
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(&jobs, nullptr, params, static_cast<int>(GetParam()));
    world.debug_set_streaming_radius_cap(2);
    entt::registry registry;
    std::mt19937 random(GetParam());
    std::int64_t tick = 0;
    int dig_changes = 0;
    int fill_changes = 0;
    // Fixture bound: sixteen activation passes at radius two, two anchors at most.
    for (int iteration = 0; iteration < 16; ++iteration) {
        SCOPED_TRACE(iteration);
        // Cross both signs of a 32-column region boundary; retain poses on alternate passes.
        const int offset = (iteration / 4) % 2 == 0 ? 31 : -33;
        const float x = static_cast<float>(offset * CHUNK_SIZE_X) + 0.25f;
        const float z =
            static_cast<float>((offset + static_cast<int>(random() % 3u)) * CHUNK_SIZE_Z) + 0.25f;
        const float height = world.GetTerrainHeightAt(x, z);
        std::vector<Vec3> anchors{Vec3(x, height + (iteration % 5 == 0 ? -80.0f : 2.0f), z)};
        if (iteration % 3 != 0)
            anchors.emplace_back(x + CHUNK_SIZE_X, height + 2.0f, z);
        if (iteration > 0) {
            auto editable = world.snapshot_streamed_chunks();
            std::sort(editable.begin(), editable.end(), [](const auto& a, const auto& b) {
                return a->get_id() < b->get_id();
            });
            for (const auto& chunk : editable) {
                if (chunk->sdf_data.empty())
                    continue;
                const Vec3 position = (Vec3(chunk->get_coords()) + 0.5f) *
                                      Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
                const bool fill = iteration % 2 == 0;
                const int edited = world.EditTerrainVoxel(position, 1.5f, fill, nullptr);
                if (edited != 0) {
                    if (fill)
                        fill_changes += edited;
                    else
                        dig_changes += edited;
                    break;
                }
            }
        }
        for (int frame = 0; frame < 3; ++frame) {
            world.begin_tick(tick++);
            world.update(registry, anchors, nullptr);
            world.wait_for_streaming_jobs();
        }
        const auto expected = FullScan(world, anchors, 2);
        std::set<ChunkID> expected_residency;
        for (const auto& chunk : world.snapshot_streamed_chunks()) {
            if (FullScanRetains(world, chunk->get_coords(), anchors, 2))
                expected_residency.insert(chunk->get_id());
        }
        world.begin_tick(tick++);
        world.update(registry, anchors, nullptr);
        const auto stats = world.get_last_streaming_budget_stats();
        EXPECT_EQ(stats.generation_candidates, expected.size());
        const auto actual = world.generation_queue_prefix(1024);
        ASSERT_EQ(actual.size(), stats.scheduled_generation);
        ASSERT_LE(actual.size(), expected.size());
        for (std::size_t index = 0; index < actual.size(); ++index) {
            EXPECT_EQ(actual[index], expected[index].coords);
            if (FullScanRetains(world, expected[index].coords, anchors, 2))
                expected_residency.insert(Chunk::calculate_id(expected[index].coords));
        }
        std::set<ChunkID> actual_residency;
        for (const auto& chunk : world.snapshot_streamed_chunks())
            actual_residency.insert(chunk->get_id());
        EXPECT_EQ(actual_residency, expected_residency);
        const auto updates_before_workers = world.activation_index_counters().index_updates;
        world.quiesce_streaming_jobs_for_save();
        EXPECT_EQ(world.activation_index_counters().index_updates, updates_before_workers);
        EXPECT_EQ(world.generation_queue_prefix(1024), actual);
        world.wait_for_streaming_jobs();
    }
    EXPECT_GT(dig_changes, 0);
    EXPECT_GT(fill_changes, 0);
}

INSTANTIATE_TEST_SUITE_P(FixedSeeds, ActivationFullScanParity, testing::Values(12345u, 424242u));

TEST(ActivationResidencyIndex, GeneratedCliffHeightmapsPreserveFullScanActivationPolicy) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    params.base_amplitude = 0.0f;
    params.height_offset = 21.0f;
    params.cliffs_enabled = true;
    params.cliff_threshold = -2.0f;
    params.cliff_step = 64.0f;
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(&jobs, nullptr, params, 12345);
    world.debug_set_streaming_radius_cap(1);
    entt::registry registry;
    for (int column = 0; column < 3; ++column) {
        const std::vector<Vec3> anchors{
            Vec3(static_cast<float>(column * CHUNK_SIZE_X) + 8.0f, 23.0f, 8.0f)};
        for (int frame = 0; frame < 3; ++frame) {
            world.begin_tick(column * 4 + frame);
            world.update(registry, anchors, nullptr);
            world.wait_for_streaming_jobs();
        }
        const auto expected = FullScan(world, anchors, 1);
        world.begin_tick(column * 4 + 3);
        world.update(registry, anchors, nullptr);
        const auto actual = world.generation_queue_prefix(1024);
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t index = 0; index < actual.size(); ++index)
            EXPECT_EQ(actual[index], expected[index].coords);
        world.wait_for_streaming_jobs();
    }
}

TEST(ActivationResidencyIndex, UnchangedWorldScansStayBoundedWhileBacklogDrains) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    params.base_amplitude = 0.0f;
    params.height_offset = 5.0f;
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(&jobs, nullptr, params, 12345);
    world.debug_set_streaming_radius_cap(8);
    entt::registry registry;
    const Vec3 anchor(0.25f, 7.0f, 0.25f);
    for (int frame = 0; frame < 4; ++frame) {
        world.begin_tick(0);
        world.update(registry, {anchor}, nullptr);
        world.quiesce_streaming_jobs_for_save();
    }
    const auto before = world.activation_index_counters();
    ASSERT_GT(before.columns_scanned, 0u);
    ASSERT_GT(before.span_cache_misses, 0u);
    ASSERT_GT(world.get_last_streaming_budget_stats().deferred_generation, 0u);
    for (int frame = 4; frame < 40; ++frame) {
        // Tick stays below publication. Workers may finish, but cannot alter
        // authority or the cache while the missing residency prefix drains.
        world.begin_tick(0);
        // Continuous motion in the same chunk must not enumerate its disc again.
        const Vec3 moving_anchor = anchor + Vec3(static_cast<float>(frame % 4) * 0.125f, 0, 0);
        world.update(registry, {moving_anchor}, nullptr);
        world.quiesce_streaming_jobs_for_save();
    }
    const auto after = world.activation_index_counters();
    // Radius eight has 197 columns. Only newly resident dirty columns need
    // one eviction visit; repeated wanted-disc and resident scans would exceed it.
    EXPECT_LE(after.columns_scanned - before.columns_scanned, 197u);
    EXPECT_EQ(after.span_cache_misses, before.span_cache_misses);
    EXPECT_GT(after.index_updates, before.index_updates);
    const auto resident = world.snapshot_streamed_chunks();
    EXPECT_EQ(resident.size(), 197u * 3u);
    EXPECT_TRUE(FullScan(world, {anchor}, 8).empty());
    for (int frame = 0; frame < 8; ++frame)
        world.update(registry, {anchor}, nullptr);
    EXPECT_EQ(world.activation_index_counters().columns_scanned, after.columns_scanned);
    EXPECT_EQ(world.activation_index_counters().index_updates, after.index_updates);
}

TEST(ActivationResidencyIndex, WorldContentChangesInvalidateSpansAtPublication) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    params.base_amplitude = 0.0f;
    params.height_offset = 5.0f;
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(&jobs, nullptr, params, 12345);
    world.debug_set_streaming_radius_cap(1);
    entt::registry registry;
    const Vec3 anchor(8.0f, 7.0f, 8.0f);
    for (int frame = 0; frame < 4; ++frame) {
        world.begin_tick(0);
        world.update(registry, {anchor}, nullptr);
    }
    const auto queued = world.generation_queue_prefix(1024);
    ASSERT_EQ(queued.size(), 15u);
    const auto dispatched = world.activation_index_counters();
    world.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(world.activation_index_counters().index_updates, dispatched.index_updates);
    world.activate_due(7);
    EXPECT_EQ(world.generation_queue_prefix(1024), queued);
    EXPECT_EQ(world.activation_index_counters().index_updates, dispatched.index_updates);
    world.activate_due(8);
    EXPECT_TRUE(world.generation_queue_prefix(1024).empty());
    EXPECT_EQ(world.activation_index_counters().index_updates,
              dispatched.index_updates + queued.size());
    const auto published = world.activation_index_counters();
    EXPECT_GT(published.span_cache_misses, dispatched.span_cache_misses);
    world.begin_tick(8);
    world.update(registry, {anchor}, nullptr);
    world.wait_for_streaming_jobs();
    EXPECT_EQ(world.activation_index_counters().span_cache_misses, published.span_cache_misses);

    const auto generated = world.activation_index_counters();
    ASSERT_GT(world.EditTerrainVoxel(Vec3(8.0f, 5.0f, 8.0f), 1.5f, false, nullptr), 0);
    EXPECT_GT(world.activation_index_counters().index_updates, generated.index_updates);
    world.update(registry, {anchor}, nullptr);
    world.wait_for_streaming_jobs();
    EXPECT_GT(world.activation_index_counters().span_cache_misses, generated.span_cache_misses);

    const auto edited = world.activation_index_counters();
    params.height_offset += 32.0f;
    world.set_params(params);
    EXPECT_GT(world.activation_index_counters().index_updates, edited.index_updates);
    for (int frame = 0; frame < 4; ++frame) {
        world.update(registry, {anchor}, nullptr);
        world.wait_for_streaming_jobs();
    }
    EXPECT_GT(world.activation_index_counters().span_cache_misses, edited.span_cache_misses);
    EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());

    // Loaded authority can carry edited heightmaps. It invalidates the cached
    // procedural span, and duplicate adoption must leave the index unchanged.
    const auto loaded = std::make_shared<Chunk>(IVec3(1, 20, 0));
    world.GenerateChunkData(*loaded);
    std::fill(loaded->heightmap_data.begin(), loaded->heightmap_data.end(), 1000.0f);
    loaded->mark_sdf_loaded_or_edited();
    loaded->set_state(ChunkState::Loading);
    const auto before_adoption = world.activation_index_counters();
    ASSERT_TRUE(world.adopt_streamed_chunk(loaded));
    EXPECT_EQ(world.activation_index_counters().index_updates, before_adoption.index_updates + 2u);
    EXPECT_FALSE(world.adopt_streamed_chunk(loaded));
    EXPECT_EQ(world.activation_index_counters().index_updates, before_adoption.index_updates + 2u);
    world.update(registry, {anchor}, nullptr);
    world.wait_for_streaming_jobs();
    EXPECT_GT(world.activation_index_counters().span_cache_misses,
              before_adoption.span_cache_misses);

    const auto before_seed = world.activation_index_counters();
    world.set_seed(424242);
    for (int frame = 0; frame < 4; ++frame) {
        world.update(registry, {anchor}, nullptr);
        world.wait_for_streaming_jobs();
    }
    EXPECT_GT(world.activation_index_counters().span_cache_misses, before_seed.span_cache_misses);
    EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());
    world.clear_world(nullptr);
    EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    for (int frame = 0; frame < 4; ++frame) {
        world.update(registry, {anchor}, nullptr);
        world.wait_for_streaming_jobs();
    }
    EXPECT_FALSE(world.snapshot_streamed_chunks().empty());
    EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());
}
TEST(ActivationResidencyIndex, CurrentRevisionPublishesCompleteGeneratedCliffSpans) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.surface_sampling_revision = Systems::kSurfaceSamplingRevision;
    params.caves_enabled = false;
    params.base_amplitude = 0.0f;
    params.height_offset = 21.0f;
    params.cliffs_enabled = true;
    params.cliff_threshold = -2.0f;
    params.cliff_step = 64.0f;
    JobSystem jobs;
    jobs.startup(1);
    SHIELD_WorldSystem world(&jobs, nullptr, params, 12345);
    world.debug_set_streaming_radius_cap(1);
    entt::registry registry;
    const Vec3 anchor(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 2.0f, 8.0f);
    // One bounded disc: five columns, each with the two vertical margins.
    for (int frame = 0; frame < 4; ++frame) {
        world.begin_tick(0);
        world.update(registry, {anchor}, nullptr);
    }
    const auto queued = world.generation_queue_prefix(1024);
    ASSERT_EQ(queued.size(), 15u);
    const auto dispatched = world.activation_index_counters();
    world.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(world.activation_index_counters().index_updates, dispatched.index_updates);
    world.activate_due(8);
    const auto published = world.activation_index_counters();
    EXPECT_EQ(published.span_cache_misses, dispatched.span_cache_misses + queued.size());
    for (int frame = 0; frame < 4; ++frame) {
        world.begin_tick(8);
        world.update(registry, {anchor}, nullptr);
        world.wait_for_streaming_jobs();
    }
    EXPECT_EQ(world.activation_index_counters().span_cache_misses, published.span_cache_misses);
    EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());
}

TEST(ActivationResidencyIndex, VoxelEditPreparationMaintainsResidencyAndPublication) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.caves_enabled = false;
    params.base_amplitude = 0.0f;
    params.height_offset = 5.0f;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 12345);
    world.debug_set_streaming_radius_cap(1);
    entt::registry registry;
    const Vec3 anchor(8.0f, 7.0f, 8.0f);
    const auto initial = world.activation_index_counters();
    EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 0.0f));
    EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 17.0f));
    EXPECT_FALSE(world.PrepareTerrainVoxelEdit(Vec3(32001.0f), 1.0f));
    EXPECT_TRUE(world.snapshot_streamed_chunks().empty());
    EXPECT_EQ(world.activation_index_counters().index_updates, initial.index_updates);

    // The sphere fits one chunk. Preparation inserts it and publishes its span
    // synchronously; subsequent activation must never leave it in the missing set.
    ASSERT_TRUE(world.PrepareTerrainVoxelEdit(anchor, 1.0f));
    ASSERT_EQ(world.snapshot_streamed_chunks().size(), 1u);
    const auto prepared = world.activation_index_counters();
    EXPECT_EQ(prepared.index_updates, initial.index_updates + 2u);
    ASSERT_TRUE(world.PrepareTerrainVoxelEdit(anchor, 1.0f));
    EXPECT_EQ(world.activation_index_counters().index_updates, prepared.index_updates);
    // Candidates are measured before generation, while FullScan below observes
    // the published result. Only the prepared chunk is resident at admission.
    const auto missing = FullScan(world, {anchor}, 1);
    ASSERT_EQ(missing.size(), 14u);
    for (int frame = 0; frame < 4; ++frame) {
        world.update(registry, {anchor}, nullptr);
        world.wait_for_streaming_jobs();
    }
    EXPECT_TRUE(FullScan(world, {anchor}, 1).empty());
    EXPECT_EQ(world.get_last_streaming_budget_stats().generation_candidates, missing.size());

    // Preparing a pre-existing authoritative full lattice must preserve both
    // the data and the cached span version.
    const auto loaded = world.find_streamed_chunk(IVec3(0));
    ASSERT_NE(loaded, nullptr);
    loaded->mark_sdf_loaded_or_edited();
    const auto saved_sdf = loaded->sdf_data;
    const auto settled = world.activation_index_counters();
    ASSERT_TRUE(world.PrepareTerrainVoxelEdit(anchor, 1.0f));
    EXPECT_EQ(loaded->sdf_data, saved_sdf);
    EXPECT_EQ(world.activation_index_counters().index_updates, settled.index_updates);
    loaded->sdf_data.resize(1);
    const auto partial_sdf = loaded->sdf_data;
    EXPECT_FALSE(world.PrepareTerrainVoxelEdit(anchor, 1.0f));
    EXPECT_EQ(loaded->sdf_data, partial_sdf);
    EXPECT_EQ(world.activation_index_counters().index_updates, settled.index_updates);
}
} // namespace
