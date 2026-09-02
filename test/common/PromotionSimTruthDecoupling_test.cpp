//  ( step 1): sim-truth publication is DECOUPLED from render
// meshing. When a coarse (surface-band-only) chunk is promoted to LOD0, its full
// voxel field (sdf/heightmap/material) must go live via the promotion lane —
// observable through wait_for_promotion_jobs — strictly BEFORE and independently
// of any render-mesh publish (current_lod flip / mesh_vertices swap). On the
// pre- code this is impossible: the voxel field is published inside the
// same process_completed_meshing_jobs body that stores current_lod=0, so the
// decoupling pin below goes RED. The trailing assertions are the standing
// regression pin: the render-mesh publish never writes sim truth.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::ChunkState;
using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::world::GameSession;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

constexpr std::size_t kFullSdfLattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                        (Luminumbra::CHUNK_SIZE_Y + 1) *
                                        (Luminumbra::CHUNK_SIZE_Z + 1);

// FNV-1a-64 over a raw byte span (the meshing_hardening_test helper).
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
    std::uint64_t hash = kFnvOffsetBasis;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

template<typename T>
std::uint64_t HashVec(const std::vector<T>& v) {
    return Fnv1a64(v.data(), v.size() * sizeof(T));
}

// Temp root containing ONLY the world preset (headless — no res/data assets).
// The root is unique per fixture instance so concurrent common_tests processes
// never share (or clobber) a directory; each TEST builds ONE instance and
// reuses it, so within-run path stability still holds. The destructor removes
// the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("luminumbra_promotion_decoupling_test_" + std::to_string(stamp));
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" /
                          "default.json",
                      root_ / "worlds" / "atlas" / "presets" / "default.json");
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

// Streaming cap (the WaterDeterminism pattern), sized for THIS test's input: it
// must find a Ready chunk that is coarse-meshed (current_lod > 0, surface-band
// only) to promote, and the LOD0 band reaches 192 m = 12 chunks — a cap at or
// below 12 would leave the whole resident disc full-detail and starve the scan.
// 16 keeps a 4-ring coarse annulus (rings 13..16, the LOD1 band) inside the
// capped disc while shedding the RENDER_DISTANCE far fill that dominated every
// wait_for_streaming_jobs tick. No assertion weakens: the byte oracle hashes
// are computed against a same-run pure regeneration of whichever chunk is
// picked (no golden constants), and the decoupling/no-mutation pins are
// properties of that one chunk's publish path, not of the resident set.
constexpr int kStreamRadiusCap = 16; // chunks (256 m): LOD0 band + coarse ring

TEST(PromotionSimTruthDecoupling, SimTruthPublishesIndependentlyOfRenderMesh) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("PromotionDecoupling", "12345", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        ASSERT_NE(world, nullptr);
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        auto& registry = session.GetRegistry();

        const Vec3 spawn = session.GetMetadata().spawnPoint;
        Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);

        // Settle residency around spawn with the full barrier so the coarse ring exists.
        for (int i = 0; i < 96; ++i) {
            world->update(registry, {anchor}, physics);
            world->wait_for_streaming_jobs();
        }

        // A promotion INPUT: Ready, coarse-meshed (current_lod > 0), surface-band
        // generated (heightmap present, sdf empty), with an active coarse render mesh.
        std::shared_ptr<Luminumbra::Chunk> target;
        for (const auto& chunk : world->snapshot_streamed_chunks()) {
            if (!chunk)
                continue;
            if (chunk->get_state() != ChunkState::Ready)
                continue;
            if (chunk->current_lod.load(std::memory_order_acquire) <= 0)
                continue;
            if (!chunk->sdf_data.empty())
                continue;
            if (chunk->heightmap_data.empty())
                continue;
            if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty())
                continue;
            target = chunk;
            break;
        }
        ASSERT_TRUE(target) << "no coarse-meshed surface chunk found to promote — "
                               "streaming settle produced no LOD>0 surface ring";

        // Move the anchor onto the target so its required LOD becomes 0.
        const auto coords = target->get_coords();
        const float px = (static_cast<float>(coords.x) + 0.5f) * Luminumbra::CHUNK_SIZE_X;
        const float pz = (static_cast<float>(coords.z) + 0.5f) * Luminumbra::CHUNK_SIZE_Z;
        const Vec3 promo_anchor(px, world->GetTerrainHeightAt(px, pz) + 2.0f, pz);

        // THE DECOUPLING PIN. Drive updates WITHOUT the meshing barrier — only the
        // promotion lane may be drained. Sim truth must go live via that lane while
        // the chunk's render state (current_lod) is still coarse. Pre-,
        // sim truth only goes live inside the render-mesh publish (which stores
        // current_lod=0 in the same body), so the lod check below fails.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        bool sim_truth_live = false;
        while (!sim_truth_live && std::chrono::steady_clock::now() < deadline) {
            world->update(registry, {promo_anchor}, physics);
            world->wait_for_promotion_jobs();
            sim_truth_live = target->sdf_data.size() == kFullSdfLattice;
            if (!sim_truth_live) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        ASSERT_TRUE(sim_truth_live)
            << "target chunk's full voxel field never went live through the promotion lane";
        EXPECT_GT(target->current_lod.load(std::memory_order_acquire), 0)
            << "current_lod is already 0 at the instant sim truth went live — the voxel "
               "field was published by the render-mesh publish, not independently of it "
               "( decoupling absent)";

        // Byte oracle: the promoted field is pure generation output, and publication
        // did not mark the chunk dirty (GenerateChunkData's contract).
        Luminumbra::Chunk reference(coords);
        world->GenerateChunkData(reference, 1);
        EXPECT_EQ(HashVec(target->sdf_data), HashVec(reference.sdf_data))
            << "promoted sdf_data != pure generation output";
        EXPECT_EQ(HashVec(target->heightmap_data), HashVec(reference.heightmap_data))
            << "promoted heightmap_data != pure generation output";
        EXPECT_EQ(HashVec(target->material_data), HashVec(reference.material_data))
            << "promoted material_data != pure generation output";
        EXPECT_FALSE(target->is_voxel_data_dirty())
            << "promotion publish left the voxel-dirty flag set";

        const std::uint64_t sdf_hash = HashVec(target->sdf_data);
        const std::uint64_t heightmap_hash = HashVec(target->heightmap_data);
        const std::uint64_t material_hash = HashVec(target->material_data);

        // The render half completes under the full barrier; the mesh publish must
        // not touch sim truth (the standing "meshing never writes sim truth" pin).
        for (int i = 0; i < 96 && target->current_lod.load(std::memory_order_acquire) != 0; ++i) {
            world->update(registry, {promo_anchor}, physics);
            world->wait_for_streaming_jobs();
        }
        EXPECT_EQ(target->current_lod.load(std::memory_order_acquire), 0)
            << "LOD0 render mesh never published after promotion";
        EXPECT_EQ(target->get_state(), ChunkState::Ready);
        EXPECT_FALSE(target->mesh_vertices.empty());
        EXPECT_EQ(HashVec(target->sdf_data), sdf_hash) << "render-mesh publish mutated sdf_data";
        EXPECT_EQ(HashVec(target->heightmap_data), heightmap_hash)
            << "render-mesh publish mutated heightmap_data";
        EXPECT_EQ(HashVec(target->material_data), material_hash)
            << "render-mesh publish mutated material_data";

        // The promotion lane actually carried the work (not some other path).
        EXPECT_GT(world->promotion_dispatch_totals().chunks, 0u)
            << "no promotion-lane dispatches recorded — the target was promoted "
               "by something other than the  promotion pipeline";
    }
    jobs.shutdown();
}

} // namespace
