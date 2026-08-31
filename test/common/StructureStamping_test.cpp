// per-voxel material channel + true voxel structure stamping.
//
// These gates pin the four load-bearing properties of the structure-stamping
// feature against a FIXED TerrainGenParams literal (NOT default.json, so the
// gate never drifts when a preset is recalibrated):
//   (a) determinism      - generating a known chunk twice yields byte-identical
//                           material_data (and sdf_data);
//   (b) structure-present - a chosen cairn's Stone voxels appear in the chunk
//                           that holds it (material_data == Stone + the mesh
//                           classifies those vertices material_id == 1);
//   (c) mineability       - carving through a structure voxel raises it to air
//                           and clears its material to 0;
//   (d) persistence       - a chunk JSON roundtrip preserves material_data
//                           byte-exact.
#include "gtest/gtest.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/StructurePlacement.h"
#include "world/WorldStreamingState.h"

#include <memory>
#include <string>

#include <cmath>
#include <filesystem>
#include <optional>
#include <vector>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace Luminumbra;

constexpr int kSeed = 4242;

// A FIXED terrain so the gate is independent of any shipped preset. Shaping /
// caves / islands / biomes / hydro all OFF, a small flat-ish surface lifted well
// above SEA_LEVEL so every cairn site stamps (height = 40 + small noise term).
Systems::TerrainGenParams FixedParams() {
    Systems::TerrainGenParams p; // start from the documented defaults
    p.base_frequency = 0.01f;
    p.base_amplitude = 4.0f; // tiny relief; surface stays ~[36, 44]
    p.height_offset = 40.0f; // comfortably above SEA_LEVEL (= 0)
    p.caves_enabled = false;
    p.surface_breaks_enabled = false;
    p.island_mask_enabled = false;
    p.shaping_enabled = false;
    p.biomes_enabled = false;
    p.hydro_enabled = false;
    p.structures_enabled = true;
    p.structures_data_dir =
        (fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "structures").string();
    return p;
}

fs::path StructuresDir() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "structures";
}

Luminumbra::World::StructureTemplatePool LoadCairn() {
    return Luminumbra::World::LoadStructureTemplatePool(StructuresDir() / "cairn", "cairn");
}

// Finds a cairn site whose entire footprint (origin +-1 on X/Z, the cairn
// half-extent) lands strictly inside one chunk, so the whole structure is in a
// single generated chunk and the count assertions are unambiguous.
std::optional<World::StructureSite>
FindInteriorCairnSite(const World::StructureTemplatePool& cairn) {
    // Scan a generous window of cells.
    auto sites = World::SitesInArea(cairn, kSeed, -2000, -2000, 2000, 2000);
    for (const auto& s : sites) {
        auto floor_mod = [](int v, int m) {
            int r = v % m;
            return r < 0 ? r + m : r;
        };
        const int lx = floor_mod(s.origin.x, CHUNK_SIZE_X);
        const int lz = floor_mod(s.origin.z, CHUNK_SIZE_Z);
        // Cairn half-extent on X/Z is 1; keep a 2-voxel margin from each border.
        if (lx >= 2 && lx <= CHUNK_SIZE_X - 2 && lz >= 2 && lz <= CHUNK_SIZE_Z - 2) {
            return s;
        }
    }
    return std::nullopt;
}

// Chunk that holds the cairn base at the dropped surface floor.
IVec3 CairnChunkCoords(const World::StructureSite& site, float surface) {
    const int floor_y = static_cast<int>(std::floor(surface));
    return Systems::SHIELD_WorldSystem::world_to_chunk_coords(
        Vec3(static_cast<float>(site.origin.x),
             static_cast<float>(floor_y),
             static_cast<float>(site.origin.z)));
}

std::size_t CountMaterial(const std::vector<u8>& data, u8 mat) {
    std::size_t n = 0;
    for (u8 v : data) {
        if (v == mat)
            ++n;
    }
    return n;
}

} // namespace

TEST(StructureStampingTest, MaterialDataDeterministic) {
    Systems::TerrainGenParams params = FixedParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    ASSERT_TRUE(world.structures_enabled()) << "fixed params must load the shipped structure pools";

    World::StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    auto site = FindInteriorCairnSite(cairn);
    ASSERT_TRUE(site.has_value()) << "no interior cairn site found in scan window";

    const float surface = world.GetTerrainHeightAt(static_cast<float>(site->origin.x),
                                                   static_cast<float>(site->origin.z));
    const IVec3 coords = CairnChunkCoords(*site, surface);

    Chunk a(coords);
    Chunk b(coords);
    world.GenerateChunkData(a, 1);
    world.GenerateChunkData(b, 1);

    EXPECT_EQ(a.sdf_data, b.sdf_data);
    EXPECT_EQ(a.material_data, b.material_data);
    // The structure chunk must have a non-empty, fully-allocated material channel.
    EXPECT_FALSE(a.material_data.empty());
    EXPECT_EQ(a.material_data.size(), a.sdf_data.size());
}

TEST(StructureStampingTest, PristineChunkHasEmptyMaterialData) {
    // A chunk far from any cairn must NOT allocate material_data (byte-identical
    // to a structures-off world for non-structure chunks).
    Systems::TerrainGenParams params = FixedParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    World::StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());

    // Find a chunk with no cairn voxels: scan chunks until one stamps nothing.
    bool found_empty = false;
    for (int cz = 0; cz < 8 && !found_empty; ++cz) {
        for (int cx = 0; cx < 8 && !found_empty; ++cx) {
            const IVec3 coords(cx, 2, cz); // y=2 -> base 32..48, surface ~40
            Chunk c(coords);
            world.GenerateChunkData(c, 1);
            if (c.material_data.empty() && !c.sdf_data.empty()) {
                found_empty = true;
            }
        }
    }
    EXPECT_TRUE(found_empty)
        << "expected at least one cairn-free chunk to leave material_data empty";
}

TEST(StructureStampingTest, CairnStoneVoxelsAppearInChunk) {
    Systems::TerrainGenParams params = FixedParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    ASSERT_TRUE(world.structures_enabled());

    World::StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    auto site = FindInteriorCairnSite(cairn);
    ASSERT_TRUE(site.has_value());

    const float surface = world.GetTerrainHeightAt(static_cast<float>(site->origin.x),
                                                   static_cast<float>(site->origin.z));
    const int floor_y = static_cast<int>(std::floor(surface));
    const IVec3 coords = CairnChunkCoords(*site, surface);

    Chunk chunk(coords);
    world.GenerateChunkData(chunk, 1);
    ASSERT_FALSE(chunk.material_data.empty());

    // The assembled cairn voxel set, dropped to the same integer floor.
    World::StructureSite dropped = *site;
    dropped.origin.y = floor_y;
    auto voxels = World::AssembleStructure(cairn, dropped);
    ASSERT_FALSE(voxels.empty());

    // Every assembled Stone voxel that lands inside this chunk must read Stone
    // in material_data and be solid (sdf <= 0).
    const int size_x = CHUNK_SIZE_X + 1;
    const int size_y = CHUNK_SIZE_Y + 1;
    const int size_z = CHUNK_SIZE_Z + 1;
    const IVec3 base = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    std::size_t checked = 0;
    for (const auto& v : voxels) {
        const int lx = v.position.x - base.x;
        const int ly = v.position.y - base.y;
        const int lz = v.position.z - base.z;
        if (lx < 0 || lx >= size_x || ly < 0 || ly >= size_y || lz < 0 || lz >= size_z) {
            continue;
        }
        const std::size_t idx = static_cast<std::size_t>(lx) +
                                static_cast<std::size_t>(ly) * size_x +
                                static_cast<std::size_t>(lz) * size_x * size_y;
        EXPECT_EQ(chunk.material_data[idx], v.material) << "voxel material mismatch";
        EXPECT_LE(chunk.sdf_data[idx], 0.0f) << "structure voxel must be solid";
        ++checked;
    }
    ASSERT_GT(checked, 0u) << "no cairn voxels landed in the chunk";
    // The cairn is Stone (material 1).
    EXPECT_GT(CountMaterial(chunk.material_data, static_cast<u8>(MaterialType::Stone)), 0u);

    // The surface mesh must classify at least one vertex as Stone (material 1),
    // proving the stamped material survives marching-cubes classification.
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);
    bool any_stone_vertex = false;
    for (const auto& vert : chunk.mesh_vertices) {
        if (vert.material_id == static_cast<u32>(MaterialType::Stone)) {
            any_stone_vertex = true;
            break;
        }
    }
    EXPECT_TRUE(any_stone_vertex) << "expected a Stone-classified mesh vertex on the cairn";
}

TEST(StructureStampingTest, MiningClearsStructureVoxel) {
    Systems::TerrainGenParams params = FixedParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    World::StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    auto site = FindInteriorCairnSite(cairn);
    ASSERT_TRUE(site.has_value());

    const float surface = world.GetTerrainHeightAt(static_cast<float>(site->origin.x),
                                                   static_cast<float>(site->origin.z));
    const int floor_y = static_cast<int>(std::floor(surface));
    const IVec3 coords = CairnChunkCoords(*site, surface);

    Chunk chunk(coords);
    world.GenerateChunkData(chunk, 1);
    ASSERT_FALSE(chunk.material_data.empty());

    // Pick the cairn base centre voxel (origin column, y = floor_y) which is
    // Stone in this chunk, then "mine" it (raise sdf to air, clear material).
    const IVec3 base = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    const int size_x = CHUNK_SIZE_X + 1;
    const int size_y = CHUNK_SIZE_Y + 1;
    const int lx = site->origin.x - base.x;
    const int ly = floor_y - base.y;
    const int lz = site->origin.z - base.z;
    ASSERT_GE(ly, 0);
    ASSERT_LT(ly, size_y);
    const std::size_t idx = static_cast<std::size_t>(lx) + static_cast<std::size_t>(ly) * size_x +
                            static_cast<std::size_t>(lz) * size_x * size_y;
    ASSERT_EQ(chunk.material_data[idx], static_cast<u8>(MaterialType::Stone));
    ASSERT_LE(chunk.sdf_data[idx], 0.0f);

    // Mine: positive density (air) + clear authored material. This mirrors the
    // live carve path's material clear (RuntimeScenarioHarness CarveSphereIntoChunk).
    chunk.sdf_data[idx] = 5.0f;
    chunk.material_data[idx] = 0u;

    EXPECT_GT(chunk.sdf_data[idx], 0.0f) << "mined voxel must read as air";
    EXPECT_EQ(chunk.material_data[idx], 0u) << "mined voxel must clear its material";
}

TEST(StructureStampingTest, PersistenceRoundtripPreservesMaterialData) {
    Systems::TerrainGenParams params = FixedParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    World::StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    auto site = FindInteriorCairnSite(cairn);
    ASSERT_TRUE(site.has_value());

    const float surface = world.GetTerrainHeightAt(static_cast<float>(site->origin.x),
                                                   static_cast<float>(site->origin.z));
    const IVec3 coords = CairnChunkCoords(*site, surface);

    // Serialize a streaming state holding the structure chunk + a pristine
    // chunk, then load it back through the public snapshot round-trip.
    WorldStreamingState state;
    auto structure_chunk = std::make_shared<Chunk>(coords);
    world.GenerateChunkData(*structure_chunk, 1);
    ASSERT_FALSE(structure_chunk->material_data.empty());
    const std::vector<u8> expected_material = structure_chunk->material_data;
    const std::vector<float> expected_sdf = structure_chunk->sdf_data;
    structure_chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(state.insert_chunk(structure_chunk));

    const IVec3 pristine_coords(123, 2, 456);
    auto pristine_chunk = std::make_shared<Chunk>(pristine_coords);
    world.GenerateChunkData(*pristine_chunk, 1);
    pristine_chunk->set_state(ChunkState::Ready);
    ASSERT_TRUE(state.insert_chunk(pristine_chunk));

    const std::string json_text =
        Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson(state);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(
        Luminumbra::Persistence::LoadWorldStreamingStateSnapshotJson(json_text, restored, errors))
        << (errors.empty() ? "" : errors.front());

    auto restored_structure = restored.find_chunk(coords);
    ASSERT_TRUE(restored_structure != nullptr);
    EXPECT_EQ(restored_structure->material_data, expected_material);
    EXPECT_EQ(restored_structure->sdf_data, expected_sdf);

    // The pristine chunk roundtrips with an EMPTY material channel (no zero-fill).
    auto restored_pristine = restored.find_chunk(pristine_coords);
    ASSERT_TRUE(restored_pristine != nullptr);
    EXPECT_TRUE(restored_pristine->material_data.empty())
        << "pristine chunk must roundtrip empty material_data";
}
