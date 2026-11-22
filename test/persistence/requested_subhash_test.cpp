#include "../support/RegionFixtureWorld.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace Luminumbra;
namespace P = Persistence;
using Mask = P::AuthoritySubHashMask;
constexpr auto kNetworkSlots = Mask::Terrain | Mask::Water | Mask::Entities;

std::shared_ptr<Chunk> PopulatedChunk(const IVec3& coords) {
    auto chunk = std::make_shared<Chunk>(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data = {-1.25f, 0.0f, 2.5f};
    chunk->heightmap_data = {1.5f, 3.0f};
    chunk->material_data = {1, 2, 3};
    chunk->water_depth_mm = {0, 100, -10};
    chunk->water_bed_mm = {-20, 30, 80};
    chunk->water_edge_flux = {1, -2, 3, -4};
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(2);
    chunk->water_mesh_generated.store(true);
    chunk->water_mesh_dirty_ticks = 3;
    chunk->max_water_delta_last_tick = 0.125f;
    chunk->ticks_below_threshold = 2;
    chunk->mesh_vertices = {{Vec3(1.0f, 2.0f, 3.0f), Vec3(0.0f, 1.0f, 0.0f), 7u}};
    chunk->mesh_indices = {0, 0, 0};
    chunk->water_mesh_vertices = chunk->mesh_vertices;
    chunk->water_mesh_indices = chunk->mesh_indices;
    chunk->pending_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_mesh_indices = chunk->mesh_indices;
    chunk->pending_water_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_water_mesh_indices = chunk->mesh_indices;
    return chunk;
}

void ExpectSelected(const P::WorldStreamingStateSubHashes& actual,
                    const P::WorldStreamingStateSubHashes& full,
                    Mask requested) {
    using Slot = std::string P::WorldStreamingStateSubHashes::*;
    const std::array<std::pair<Mask, Slot>, 8> slots{{
        {Mask::Terrain, &P::WorldStreamingStateSubHashes::terrain},
        {Mask::Mesh, &P::WorldStreamingStateSubHashes::mesh},
        {Mask::Water, &P::WorldStreamingStateSubHashes::water},
        {Mask::Entities, &P::WorldStreamingStateSubHashes::entities},
        {Mask::Wind, &P::WorldStreamingStateSubHashes::wind},
        {Mask::Weather, &P::WorldStreamingStateSubHashes::weather},
        {Mask::Aether, &P::WorldStreamingStateSubHashes::aether},
        {Mask::AetherState, &P::WorldStreamingStateSubHashes::aether_state},
    }};
    for (const auto& [mask, member] : slots) {
        EXPECT_EQ(actual.*member, P::RequestsSubHash(requested, mask) ? full.*member : "");
    }
}

TEST(RequestedSubHashes, EveryMaskKeepsRequestedBytesAcrossFixturesAndCursors) {
    for (const int fixture : {0, 1, 2}) {
        WorldStreamingState state;
        if (fixture != 0) {
            ASSERT_TRUE(state.insert_chunk(PopulatedChunk(IVec3(-2, 0, 3))));
        }
        if (fixture == 2) {
            auto empty_chunk = std::make_shared<Chunk>(IVec3(4, -1, 0));
            empty_chunk->set_state(ChunkState::Ready);
            ASSERT_TRUE(state.insert_chunk(empty_chunk));
        }
        const auto snapshot = P::SerializeWorldStreamingStateSnapshotJson(state);
        for (const auto cursor :
             {std::uint64_t{0}, std::uint64_t{65}, std::numeric_limits<std::uint64_t>::max()}) {
            SCOPED_TRACE(fixture);
            SCOPED_TRACE(cursor);
            const auto full = P::ComputeWorldStreamingStateSubHashes(state, "entities", cursor);
            // The mask has eight defined bits: exhaust all 256 combinations.
            for (std::uint16_t bits = 0; bits <= 0xffu; ++bits) {
                SCOPED_TRACE(bits);
                const auto requested = static_cast<Mask>(bits);
                const auto sub =
                    P::ComputeWorldStreamingStateSubHashes(state, "entities", cursor, requested);
                ExpectSelected(sub, full, requested);
                const auto chunks_only =
                    P::ComputeWorldStreamingStateSubHashes(state, cursor, requested);
                EXPECT_EQ(chunks_only.terrain, sub.terrain);
                EXPECT_EQ(chunks_only.mesh, sub.mesh);
                EXPECT_EQ(chunks_only.water, sub.water);
                EXPECT_TRUE(chunks_only.entities.empty());
            }
        }
        EXPECT_EQ(P::SerializeWorldStreamingStateSnapshotJson(state), snapshot);
    }
}

TEST(RequestedSubHashes, NetworkCaptureSerializesZeroMeshElementsAndKeepsWorldBytes) {
    WorldStreamingState state;
    ASSERT_TRUE(state.insert_chunk(PopulatedChunk(IVec3(0, 0, 0))));
    const P::AuthorityRuntimeHashes runtime{
        "wind", "weather", "aether", "scent", "ecology", "plant", "aether-state"};
    P::AuthorityRegionalInputs regional;
    regional.require_index = false;
    regional.regions.push_back({{0, 0}, {}, std::uint64_t{42}});
    for (const auto parts : {P::AuthorityCaptureParts::All,
                             P::AuthorityCaptureParts::StreamedAndSubHashes,
                             P::AuthorityCaptureParts::SubHashes,
                             P::AuthorityCaptureParts::Composite,
                             P::AuthorityCaptureParts::Chunk}) {
        for (const bool regions : {false, true}) {
            const auto capture = [&](Mask requested, P::AuthorityHashWork& work) {
                return regions
                           ? P::CaptureAuthorityHashes(
                                 state, 65, regional, "entities", runtime, parts, requested, &work)
                           : P::CaptureAuthorityHashes(state,
                                                       65,
                                                       std::nullopt,
                                                       "entities",
                                                       runtime,
                                                       parts,
                                                       requested,
                                                       &work);
            };
            P::AuthorityHashWork full_work;
            const auto full = capture(Mask::All, full_work);
            ASSERT_TRUE(full.capture.has_value());
            const bool sub = parts == P::AuthorityCaptureParts::All ||
                             parts == P::AuthorityCaptureParts::SubHashes ||
                             parts == P::AuthorityCaptureParts::StreamedAndSubHashes;
            // Four vertex elements and twelve index elements across all eight arrays.
            EXPECT_EQ(full_work.serialized_mesh_elements, sub ? 16u : 0u);
            P::AuthorityHashWork network_work;
            const auto network = capture(kNetworkSlots, network_work);
            ASSERT_TRUE(network.capture.has_value());
            EXPECT_EQ(network_work.serialized_mesh_elements, 0u);
            EXPECT_EQ(network.capture->streamed, full.capture->streamed);
            EXPECT_EQ(network.capture->chunk, full.capture->chunk);
            EXPECT_EQ(network.capture->composite, full.capture->composite);
            ExpectSelected(network.capture->sub, full.capture->sub, kNetworkSlots);
            // Repeated captures accumulate actual work without resetting the diagnostic.
            ASSERT_TRUE(capture(Mask::All, network_work).capture.has_value());
            EXPECT_EQ(network_work.serialized_mesh_elements, full_work.serialized_mesh_elements);
        }
    }
}

TEST(RequestedSubHashes, SessionSkipsUnrequestedRuntimeWorkAndPreservesCompositeDependencies) {
    TestSupport::RegionFixtureWorld fixture;
    world::GameSession session;
    fixture.Configure(session, true);
    EXPECT_TRUE(session.ComputeAetherStateSubHash().empty());
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Hash selection", "1337", "default"));
    ASSERT_TRUE(session.LoadWorldStateFrom(fixture.path() / "unmarked-snapshot"));
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    energy->SetAnchorCell(0, 0);
    energy->QueueDeposit(1, 0, 0, 0, 512);
    energy->Tick(1);
    ASSERT_FALSE(session.ComputeAetherStateSubHash().empty());
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(PopulatedChunk(IVec3(0, 0, 0))));
    const auto full = session.CaptureAuthorityHashes("entities");
    ASSERT_TRUE(full.capture.has_value());
    for (const auto parts : {P::AuthorityCaptureParts::StreamedAndSubHashes,
                             P::AuthorityCaptureParts::SubHashes,
                             P::AuthorityCaptureParts::All,
                             P::AuthorityCaptureParts::Composite,
                             P::AuthorityCaptureParts::Chunk}) {
        P::AuthorityHashWork work;
        const auto selected =
            session.CaptureAuthorityHashes("entities", parts, kNetworkSlots, &work);
        ASSERT_TRUE(selected.capture.has_value());
        EXPECT_EQ(work.serialized_mesh_elements, 0u);
        const bool composite =
            parts == P::AuthorityCaptureParts::All || parts == P::AuthorityCaptureParts::Composite;
        if (composite) {
            EXPECT_GT(work.runtime_hashes, 0u);
            EXPECT_EQ(selected.capture->composite, full.capture->composite);
        } else {
            EXPECT_EQ(work.runtime_hashes, 0u);
        }
        const bool sub = parts != P::AuthorityCaptureParts::Composite &&
                         parts != P::AuthorityCaptureParts::Chunk;
        ExpectSelected(selected.capture->sub, full.capture->sub, sub ? kNetworkSlots : Mask::None);
    }
    P::AuthorityHashWork refused_work;
    const auto refused = session.CaptureAuthorityHashes(
        "entities", P::AuthorityCaptureParts::All, static_cast<Mask>(0x100u), &refused_work);
    EXPECT_EQ(refused.status, P::AuthorityCaptureStatus::PreparationRefused);
    EXPECT_FALSE(refused.capture.has_value());
    EXPECT_EQ(refused_work.runtime_hashes, 0u);
    EXPECT_EQ(refused_work.serialized_mesh_elements, 0u);
    const auto repeated = session.CaptureAuthorityHashes("entities");
    ASSERT_TRUE(repeated.capture.has_value());
    EXPECT_EQ(repeated.capture->composite, full.capture->composite);
    ExpectSelected(repeated.capture->sub, full.capture->sub, Mask::All);
    // Runtime singleton masks exercise the state-only aether dependency separately.
    for (const auto mask :
         {Mask::Wind, Mask::Weather, Mask::Aether, Mask::AetherState, Mask::None, Mask::All}) {
        const auto selected =
            session.CaptureAuthorityHashes("entities", P::AuthorityCaptureParts::SubHashes, mask);
        ASSERT_TRUE(selected.capture.has_value());
        ExpectSelected(selected.capture->sub, full.capture->sub, mask);
    }
}

TEST(RequestedSubHashes, InvalidMasksAndUnavailableInputsPublishNoCapture) {
    WorldStreamingState state;
    auto chunk = PopulatedChunk(IVec3(0, 0, 0));
    ASSERT_TRUE(state.insert_chunk(chunk));
    P::AuthorityRegionalInputs regional;
    const auto invalid = static_cast<Mask>(0x100u);
    EXPECT_THROW(P::ComputeWorldStreamingStateSubHashes(state, 0, invalid), std::invalid_argument);
    EXPECT_THROW(P::ComputeWorldStreamingStateSubHashes(state, "", 0, invalid),
                 std::invalid_argument);
    world::GameSession absent;
    for (const auto mask : {Mask::None, kNetworkSlots, invalid}) {
        P::AuthorityHashWork work;
        const auto no_session = absent.CaptureAuthorityHashes(
            {}, P::AuthorityCaptureParts::StreamedAndSubHashes, mask, &work);
        EXPECT_FALSE(no_session.capture.has_value());
        EXPECT_EQ(no_session.status,
                  mask == invalid ? P::AuthorityCaptureStatus::PreparationRefused
                                  : P::AuthorityCaptureStatus::MissingCursor);
        for (const bool regions : {false, true}) {
            const auto capture = [&](std::optional<std::uint64_t> cursor) {
                return regions ? P::CaptureAuthorityHashes(state,
                                                           cursor,
                                                           regional,
                                                           {},
                                                           {},
                                                           P::AuthorityCaptureParts::All,
                                                           mask,
                                                           &work)
                               : P::CaptureAuthorityHashes(state,
                                                           cursor,
                                                           std::nullopt,
                                                           {},
                                                           {},
                                                           P::AuthorityCaptureParts::All,
                                                           mask,
                                                           &work);
            };
            EXPECT_FALSE(capture(std::nullopt).capture.has_value());
            if (mask == invalid) {
                EXPECT_FALSE(capture(0).capture.has_value());
            } else {
                chunk->mark_voxel_data_dirty();
                chunk->set_state(ChunkState::Loading);
                EXPECT_EQ(capture(0).status, P::AuthorityCaptureStatus::IncompleteOwner);
                chunk->set_state(ChunkState::Idle);
            }
        }
        EXPECT_EQ(work.serialized_mesh_elements, 0u);
        EXPECT_EQ(work.runtime_hashes, 0u);
    }
}
} // namespace
