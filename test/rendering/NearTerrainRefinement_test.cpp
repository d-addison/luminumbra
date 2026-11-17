#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_client/rendering/NearTerrainRefinement.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/world/NearTerrainMeshSnapshot.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;

float RiseHeight(float x, float z) {
    return 4.5f - 0.125f * ((x - 8.0f) * (x - 8.0f) + (z - 8.0f) * (z - 8.0f));
}

struct Patch {
    NearTerrainRenderMesh mesh;
    std::vector<float> densities;

    NearTerrainRefinementInput Input() const {
        return {.vertices = mesh.vertices,
                .indices = mesh.indices,
                .densities = densities,
                .sample_step = 1,
                .chunk_distance_squared = 0.0f,
                .enabled = true,
                .generated_current_params = true,
                .unedited_halo = true};
    }
};

Patch MakePatch(int cells = 4, bool flat = false) {
    Patch patch;
    const float spacing = flat ? 16.0f / static_cast<float>(cells) : 1.0f;
    const float origin = flat ? 0.0f : 6.0f;
    for (int z = 0; z <= cells; ++z) {
        for (int x = 0; x <= cells; ++x) {
            const float px = origin + static_cast<float>(x) * spacing;
            const float pz = origin + static_cast<float>(z) * spacing;
            patch.mesh.vertices.push_back(
                {{px, flat ? 8.0f : RiseHeight(px, pz), pz},
                 flat ? Vec3(0, 1, 0)
                      : glm::normalize(Vec3(0.25f * (px - 8.0f), 1, 0.25f * (pz - 8.0f))),
                 static_cast<u32>((x + z) % 3 + 1)});
        }
    }
    for (int z = 0; z < cells; ++z) {
        for (int x = 0; x < cells; ++x) {
            const auto a = static_cast<u32>(x + z * (cells + 1));
            const auto b = a + 1;
            const auto c = a + static_cast<u32>(cells + 1);
            patch.mesh.indices.insert(patch.mesh.indices.end(), {a, c, b, b, c, c + 1});
        }
    }
    for (int z = 0; z <= 16; ++z) {
        for (int y = 0; y <= 16; ++y) {
            for (int x = 0; x <= 16; ++x) {
                patch.densities.push_back(
                    static_cast<float>(y) -
                    (flat ? 8.0f : RiseHeight(static_cast<float>(x), static_cast<float>(z))));
            }
        }
    }
    return patch;
}

void ExpectSameMesh(const NearTerrainRenderMesh& actual, const NearTerrainRenderMesh& expected) {
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    EXPECT_EQ(actual.indices, expected.indices);
    for (std::size_t i = 0; i < actual.vertices.size(); ++i) {
        EXPECT_EQ(actual.vertices[i].position, expected.vertices[i].position);
        EXPECT_EQ(actual.vertices[i].normal, expected.vertices[i].normal);
        EXPECT_EQ(actual.vertices[i].material_id, expected.vertices[i].material_id);
    }
}

void ExpectRefusal(const NearTerrainRefinementInput& input, NearTerrainRefinementResult expected) {
    const auto prior = MakePatch().mesh;
    auto output = prior;
    EXPECT_EQ(RefineNearTerrain(input, output), expected);
    ExpectSameMesh(output, prior);
}

float MaxRiseDeviation(const NearTerrainRenderMesh& mesh) {
    float deviation = 0;
    for (std::size_t start = 0; start < mesh.indices.size(); start += 3) {
        const Vec3 centre = (mesh.vertices[mesh.indices[start]].position +
                             mesh.vertices[mesh.indices[start + 1]].position +
                             mesh.vertices[mesh.indices[start + 2]].position) /
                            3.0f;
        deviation = std::max(deviation, std::abs(centre.y - RiseHeight(centre.x, centre.z)));
    }
    return deviation;
}

using EdgeCounts = std::map<std::pair<u32, u32>, unsigned>;
EdgeCounts CountEdges(const NearTerrainRenderMesh& mesh) {
    EdgeCounts counts;
    for (std::size_t start = 0; start < mesh.indices.size(); start += 3) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto a = mesh.indices[start + corner];
            const auto b = mesh.indices[start + (corner + 1) % 3];
            ++counts[{std::min(a, b), std::max(a, b)}];
        }
    }
    return counts;
}
} // namespace

TEST(NearTerrainRefinement, RoundsShortRiseAndKeepsSourceImmutable) {
    const auto patch = MakePatch();
    const auto original = patch.mesh;
    NearTerrainRenderMesh output;
    ASSERT_EQ(RefineNearTerrain(patch.Input(), output), NearTerrainRefinementResult::Refined);
    EXPECT_GT(output.indices.size(), original.indices.size());
    EXPECT_GT(MaxRiseDeviation(original), 0.05f);
    EXPECT_LT(MaxRiseDeviation(output), MaxRiseDeviation(original) * 0.85f);
    ExpectSameMesh(patch.mesh, original);
    for (std::size_t i = 0; i < original.vertices.size(); ++i) {
        EXPECT_EQ(output.vertices[i].position, original.vertices[i].position);
        EXPECT_EQ(output.vertices[i].normal, original.vertices[i].normal);
        EXPECT_EQ(output.vertices[i].material_id, original.vertices[i].material_id);
    }
    for (std::size_t start = 0; start < output.indices.size(); start += 3) {
        const auto& a = output.vertices[output.indices[start]].position;
        const auto& b = output.vertices[output.indices[start + 1]].position;
        const auto& c = output.vertices[output.indices[start + 2]].position;
        EXPECT_GT(glm::cross(b - a, c - a).y, 0.0f);
    }
}

TEST(NearTerrainRefinement, EditedCaveAndMultipleSurfacePatchesStayUntouched) {
    const auto pristine = MakePatch();
    auto edited = pristine.Input();
    edited.generated_current_params = false;
    ExpectRefusal(edited, NearTerrainRefinementResult::Ineligible);

    auto cave = pristine;
    cave.densities[8 + 8 * 17 + 8 * 17 * 17] = -1.0f;
    ExpectRefusal(cave.Input(), NearTerrainRefinementResult::Ineligible);

    auto multiple = pristine;
    multiple.densities[8 + 5 * 17 + 8 * 17 * 17] = 1.0f;
    multiple.densities[8 + 6 * 17 + 8 * 17 * 17] = -1.0f;
    multiple.densities[8 + 7 * 17 + 8 * 17 * 17] = 1.0f;
    ExpectRefusal(multiple.Input(), NearTerrainRefinementResult::Ineligible);
}

TEST(NearTerrainRefinement, MissingStaleFarAndCoarseEligibilityRefuses) {
    const auto patch = MakePatch();
    std::array<NearTerrainRefinementInput, 14> cases;
    cases.fill(patch.Input());
    cases[0].enabled = false;
    cases[1].generated_current_params = false;
    cases[2].unedited_halo = false;
    cases[3].sample_step = 0;
    cases[4].sample_step = 2;
    cases[5].sample_step = 4;
    cases[6].chunk_distance_squared = -1;
    cases[7].chunk_distance_squared = 1024.01f;
    cases[8].chunk_distance_squared = std::numeric_limits<float>::infinity();
    cases[9].chunk_distance_squared = std::numeric_limits<float>::quiet_NaN();
    cases[10].densities = {};
    cases[11].densities = std::span<const float>(patch.densities).first(4912);
    auto oversized = patch.densities;
    oversized.push_back(0);
    cases[12].densities = oversized;
    cases[13].sample_step = -1;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE(i);
        ExpectRefusal(cases[i], NearTerrainRefinementResult::Ineligible);
    }
    auto at_radius = patch.Input();
    at_radius.chunk_distance_squared = 1024;
    NearTerrainRenderMesh output;
    EXPECT_EQ(RefineNearTerrain(at_radius, output), NearTerrainRefinementResult::Refined);
    ExpectRefusal(NearTerrainRefinementInput{}, NearTerrainRefinementResult::Ineligible);
}

TEST(NearTerrainRefinement, InvalidDensitiesAtEveryVerticalBoundaryRefuse) {
    for (const auto index : {0u, 16u * 17u, 4912u}) {
        for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity(),
                                  123.0f}) {
            auto patch = MakePatch();
            patch.densities[index] = value;
            ExpectRefusal(patch.Input(), NearTerrainRefinementResult::Ineligible);
        }
    }
}

TEST(NearTerrainRefinement, OverhangsInvalidGeometryAndTopologyPreservePriorOutput) {
    for (int variant = 0; variant < 19; ++variant) {
        SCOPED_TRACE(variant);
        auto patch = MakePatch();
        switch (variant) {
            case 0:
                patch.mesh.vertices[0].normal = Vec3(0, -1, 0);
                break;
            case 1:
                patch.mesh.vertices[0].normal = glm::normalize(Vec3(1, 0.1f, 0));
                break;
            case 2:
                patch.mesh.vertices[0].normal = Vec3(0);
                break;
            case 3:
                patch.mesh.vertices[0].normal.x = std::numeric_limits<float>::quiet_NaN();
                break;
            case 4:
                patch.mesh.vertices[0].normal.x = std::numeric_limits<float>::max();
                break;
            case 5:
                patch.mesh.vertices[0].position.x = -1;
                break;
            case 6:
                patch.mesh.vertices[0].position.y = 17;
                break;
            case 7:
                patch.mesh.vertices[0].position.z = std::numeric_limits<float>::infinity();
                break;
            case 8:
                patch.mesh.indices[0] = std::numeric_limits<u32>::max();
                break;
            case 9:
                patch.mesh.indices.pop_back();
                break;
            case 10:
                patch.mesh.indices.clear();
                break;
            case 11:
                patch.mesh.vertices.clear();
                break;
            case 12:
                patch.mesh.indices[0] = patch.mesh.indices[1];
                break;
            case 13:
                patch.mesh.indices = {0, 5, 1, 0, 5, 1};
                break;
            case 14:
                patch.mesh.indices = {0, 5, 1, 0, 5, 2, 0, 5, 3};
                break;
            case 15:
                std::swap(patch.mesh.indices[0], patch.mesh.indices[1]);
                break;
            case 16:
                patch.mesh.vertices[0].normal = Vec3(0, 0.5f, 0);
                break;
            case 17:
                patch.mesh.vertices[0].position.y = 16;
                break;
            case 18:
                patch.mesh.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
                break;
        }
        ExpectRefusal(patch.Input(), NearTerrainRefinementResult::InvalidMesh);
    }
}

TEST(NearTerrainRefinement, UnrefinedNeighbourAndLodBorderEdgesMatchExactly) {
    auto left = MakePatch();
    auto right = MakePatch();
    for (auto& vertex : left.mesh.vertices) {
        vertex.position.x += 6;
    }
    for (auto& vertex : right.mesh.vertices) {
        vertex.position.x -= 6;
    }
    NearTerrainRenderMesh refined;
    ASSERT_EQ(RefineNearTerrain(left.Input(), refined), NearTerrainRefinementResult::Refined);
    const auto original_edges = CountEdges(left.mesh);
    const auto refined_edges = CountEdges(refined);
    for (const auto& [edge, count] : original_edges) {
        if (count == 1) {
            const auto found = refined_edges.find(edge);
            ASSERT_NE(found, refined_edges.end());
            EXPECT_EQ(found->second, 1u);
        }
    }
    for (const auto& [edge, count] : refined_edges) {
        if (count == 1) {
            const auto found = original_edges.find(edge);
            ASSERT_NE(found, original_edges.end());
            EXPECT_EQ(found->second, 1u);
        } else {
            EXPECT_EQ(count, 2u);
        }
    }
    for (const int neighbour_step : {1, 2}) {
        auto unrefined = right.mesh;
        auto input = right.Input();
        input.sample_step = neighbour_step;
        input.enabled = neighbour_step != 1;
        ASSERT_EQ(RefineNearTerrain(input, unrefined), NearTerrainRefinementResult::Ineligible);
        for (int z = 0; z <= 4; ++z) {
            const auto left_index = static_cast<std::size_t>(4 + z * 5);
            const auto right_index = static_cast<std::size_t>(z * 5);
            EXPECT_EQ(refined.vertices[left_index].position,
                      unrefined.vertices[right_index].position + Vec3(16, 0, 0));
        }
    }
    for (std::size_t i = left.mesh.vertices.size(); i < refined.vertices.size(); ++i) {
        EXPECT_LT(refined.vertices[i].position.x, 16.0f);
    }
}

TEST(NearTerrainRefinement, SharedHorizontalChunkFacesRemainPlanar) {
    for (const float face : {0.0f, 16.0f}) {
        auto patch = MakePatch();
        for (auto& vertex : patch.mesh.vertices) {
            vertex.position.y = face;
        }
        NearTerrainRenderMesh output;
        ASSERT_EQ(RefineNearTerrain(patch.Input(), output), NearTerrainRefinementResult::Refined);
        EXPECT_EQ(output.indices.size(), patch.mesh.indices.size() * 3);
        for (const auto& vertex : output.vertices) {
            EXPECT_EQ(vertex.position.y, face);
        }
    }
}

TEST(NearTerrainRefinement, TriangleMemoryAndDisplacementCapsHoldAtAdmissionLimit) {
    const auto patch = MakePatch(32, true);
    ASSERT_EQ(patch.mesh.indices.size() / 3, kNearTerrainMaxInputTriangles);
    NearTerrainRenderMesh output;
    ASSERT_EQ(RefineNearTerrain(patch.Input(), output), NearTerrainRefinementResult::Refined);
    EXPECT_LE(output.indices.size() / 3, kNearTerrainMaxOutputTriangles);
    EXPECT_LE(output.vertices.size(), patch.mesh.vertices.size() + kNearTerrainMaxAddedVertices);
    EXPECT_LE(output.vertices.capacity(),
              kNearTerrainMaxInputVertices + kNearTerrainMaxAddedVertices);
    EXPECT_LE(output.indices.capacity(), kNearTerrainMaxOutputTriangles * 3);

    auto too_many = patch;
    too_many.mesh.indices.insert(too_many.mesh.indices.end(), {0, 33, 1});
    ExpectRefusal(too_many.Input(), NearTerrainRefinementResult::CapacityExceeded);
    auto too_many_vertices = MakePatch();
    too_many_vertices.mesh.vertices.resize(kNearTerrainMaxInputVertices + 1);
    ExpectRefusal(too_many_vertices.Input(), NearTerrainRefinementResult::CapacityExceeded);

    auto sharp = MakePatch();
    for (auto& vertex : sharp.mesh.vertices) {
        vertex.position.y = 8;
        const float sign = vertex.position.x < 8 ? -1.0f : 1.0f;
        vertex.normal = glm::normalize(Vec3(3.0f * sign, 1, 0));
    }
    ASSERT_EQ(RefineNearTerrain(sharp.Input(), output), NearTerrainRefinementResult::Refined);
    float max_displacement = 0;
    for (const auto& vertex : output.vertices) {
        const float displacement = std::abs(vertex.position.y - 8.0f);
        EXPECT_LE(displacement, kNearTerrainMaxDisplacementMeters);
        max_displacement = std::max(max_displacement, displacement);
    }
    EXPECT_EQ(max_displacement, kNearTerrainMaxDisplacementMeters);
}

TEST(NearTerrainRefinement, RepeatedAndAliasedReplacementIsDeterministic) {
    const auto patch = MakePatch();
    NearTerrainRenderMesh first;
    NearTerrainRenderMesh second;
    ASSERT_EQ(RefineNearTerrain(patch.Input(), first), NearTerrainRefinementResult::Refined);
    ASSERT_EQ(RefineNearTerrain(patch.Input(), second), NearTerrainRefinementResult::Refined);
    ExpectSameMesh(first, second);
    auto aliased = patch.mesh;
    auto input = patch.Input();
    input.vertices = aliased.vertices;
    input.indices = aliased.indices;
    ASSERT_EQ(RefineNearTerrain(input, aliased), NearTerrainRefinementResult::Refined);
    ExpectSameMesh(aliased, first);
}

TEST(NearTerrainRefinement, EveryChildPreservesFlatProvokingVertexMaterial) {
    const auto patch = MakePatch();
    NearTerrainRenderMesh output;
    ASSERT_EQ(RefineNearTerrain(patch.Input(), output), NearTerrainRefinementResult::Refined);
    const auto first_centre = output.vertices.size() - patch.mesh.indices.size() / 3;
    for (std::size_t start = 0; start < output.indices.size(); start += 3) {
        const auto provoking = output.indices[start + 2];
        ASSERT_GE(provoking, first_centre);
        ASSERT_LT(provoking, output.vertices.size());
        const auto source_triangle = provoking - first_centre;
        const auto source_provoking = patch.mesh.indices[source_triangle * 3 + 2];
        EXPECT_EQ(output.vertices[provoking].material_id,
                  patch.mesh.vertices[source_provoking].material_id);
    }
}

namespace {
struct EligibilityPatch {
    std::array<std::shared_ptr<Chunk>, 27> chunks;
    World::NearTerrainMeshSnapshot evidence;

    EligibilityPatch() {
        std::size_t index = 0;
        for (int z = -1; z <= 1; ++z) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x, ++index) {
                    chunks[index] = std::make_shared<Chunk>(IVec3(x, y, z));
                    evidence.halo[index] = {chunks[index], 0};
                }
            }
        }
        evidence.mesh_version = 7;
        evidence.sample_step = 1;
        evidence.generated_current_params = true;
        evidence.unedited_halo = true;
    }

    std::shared_ptr<const World::NearTerrainMeshSnapshot> Publish(bool retain_densities = true) {
        auto densities = MakePatch().densities;
        chunks[13]->sdf_data = densities;
        const auto snapshot =
            World::CompleteNearTerrainMeshSnapshot(evidence, densities, retain_densities);
        // Completion retains densities only for eligible step-1, current, unedited evidence.
        const bool retained = retain_densities && evidence.sample_step == 1 &&
                              evidence.generated_current_params && evidence.unedited_halo;
        EXPECT_EQ(snapshot->densities.size(), retained ? World::kNearTerrainDensityCount : 0u);
        chunks[13]->mesh_version.store(evidence.mesh_version);
        chunks[13]->near_terrain_snapshot.store(snapshot);
        return snapshot;
    }
};
} // namespace

TEST(NearTerrainEligibility, PublishesMatchingMeshVersionAndOwnsItsLattice) {
    EligibilityPatch patch;
    auto densities = MakePatch().densities;
    const auto expected = densities;
    const auto snapshot = World::CompleteNearTerrainMeshSnapshot(patch.evidence, densities, true);
    auto& chunk = *patch.chunks[13];
    chunk.near_terrain_snapshot.store(snapshot);
    EXPECT_EQ(World::CurrentNearTerrainMeshSnapshot(chunk, 7), nullptr);
    chunk.mesh_version.store(7);
    EXPECT_EQ(World::CurrentNearTerrainMeshSnapshot(chunk, 7), snapshot);
    EXPECT_EQ(snapshot->densities, expected);
    EXPECT_EQ(snapshot->sample_step, 1);
    EXPECT_TRUE(snapshot->generated_current_params);
    EXPECT_TRUE(snapshot->unedited_halo);
    EXPECT_TRUE(snapshot->complete_height_like_sdf);
    EXPECT_TRUE(snapshot->eligible(chunk, 7, 0));
    EXPECT_TRUE(snapshot->eligible(chunk, 7, 1024));
    EXPECT_FALSE(snapshot->eligible(chunk, 7, 1024.01f));
    EXPECT_FALSE(snapshot->eligible(chunk, 7, -1));
    EXPECT_FALSE(snapshot->eligible(chunk, 7, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(snapshot->eligible(chunk, 7, std::numeric_limits<float>::quiet_NaN()));
}

TEST(NearTerrainEligibility, EveryEditInvalidatesSelfAndAllHaloNeighbours) {
    // Relative halo membership is symmetric: changing any one of these 27
    // chunks invalidates the source's evidence, including corner neighbours.
    for (std::size_t index = 0; index < 27; ++index) {
        for (int edit = 0; edit < 3; ++edit) {
            SCOPED_TRACE(index);
            SCOPED_TRACE(edit);
            EligibilityPatch patch;
            const auto snapshot = patch.Publish();
            ASSERT_NE(World::CurrentNearTerrainMeshSnapshot(*patch.chunks[13], 7), nullptr);
            if (edit == 0) {
                patch.chunks[index]->mark_voxel_data_dirty();
            } else if (edit == 1) {
                patch.chunks[index]->mark_sdf_loaded_or_edited();
            } else {
                patch.chunks[index]->invalidate_near_terrain_snapshot();
            }
            EXPECT_EQ(World::CurrentNearTerrainMeshSnapshot(*patch.chunks[13], 7), nullptr);
            EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
            patch.chunks[index]->clear_voxel_data_dirty();
            patch.chunks[index]->mark_sdf_generated_current_params();
            EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
        }
    }
}

TEST(NearTerrainEligibility, WaterPromotionInvalidatesSourceAndEveryHaloNeighbour) {
    for (std::size_t index = 0; index < 27; ++index) {
        for (bool publish : {false, true}) {
            SCOPED_TRACE(index);
            SCOPED_TRACE(publish);
            EligibilityPatch patch;
            const auto snapshot = patch.Publish();
            auto& promoted = *patch.chunks[index];
            ASSERT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
            if (publish) {
                promoted.publish_water_state();
            } else {
                promoted.set_water_authority();
            }
            EXPECT_FALSE(snapshot->matches(*patch.chunks[13], 7));
            EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
            EXPECT_EQ(promoted.sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
            EXPECT_EQ(promoted.voxel_revision(), 1u);
            const auto epoch = promoted.near_terrain_epoch.load();
            promoted.set_water_authority();
            EXPECT_EQ(promoted.near_terrain_epoch.load(), epoch);
            EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
        }
    }
    Chunk edited(IVec3(0));
    edited.mark_voxel_data_dirty();
    const auto epoch = edited.near_terrain_epoch.load();
    edited.set_water_authority();
    EXPECT_EQ(edited.near_terrain_epoch.load(), epoch);
}

TEST(NearTerrainEligibility, ResidentRemovalRefusesRetainedOwnersAfterReplacement) {
    for (std::size_t index = 0; index < 27; ++index) {
        for (int removal = 0; removal < 3; ++removal) {
            SCOPED_TRACE(index);
            SCOPED_TRACE(removal);
            EligibilityPatch patch;
            auto state = std::make_unique<WorldStreamingState>();
            for (const auto& chunk : patch.chunks) {
                ASSERT_TRUE(state->insert_chunk(chunk));
            }
            const auto snapshot = patch.Publish();
            EXPECT_FALSE(state->insert_chunk(nullptr));
            EXPECT_FALSE(state->insert_chunk(patch.chunks[index]));
            EXPECT_FALSE(state->erase_chunk(IVec3(10)));
            ASSERT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
            const auto& retired = patch.chunks[index];
            if (removal == 0) {
                ASSERT_TRUE(state->erase_chunk(retired->get_id()));
                EXPECT_FALSE(state->erase_chunk(retired->get_id()));
            } else if (removal == 1) {
                ASSERT_TRUE(state->erase_chunk(retired->get_coords()));
            } else {
                state->clear();
                state->clear();
            }
            const auto replacement = state->get_or_create_chunk(retired->get_coords());
            ASSERT_NE(replacement, retired);
            EXPECT_EQ(state->get_or_create_chunk(retired->get_coords()), replacement);
            EXPECT_EQ(state->find_chunk(retired->get_coords()), replacement);
            // Every old owner remains strongly held by patch, including the source.
            EXPECT_FALSE(snapshot->matches(*patch.chunks[13], 7));
            EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
        }
    }
}

TEST(NearTerrainEligibility, LiveWorldEvictionRefusesRetainedOwnersAfterReplacement) {
    for (int removal = 0; removal < 4; ++removal) {
        SCOPED_TRACE(removal);
        EligibilityPatch patch;
        auto world = std::make_unique<Systems::SHIELD_WorldSystem>(
            nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
        world->debug_set_streaming_radius_cap(1);
        world->set_durable_residency(removal == 1);
        for (std::size_t index = 0; index < 27; ++index) {
            patch.chunks[index]->set_state(ChunkState::Ready);
            patch.chunks[index]->current_lod.store(0);
            // A pending remesh must not refresh the evidence before eviction.
            patch.chunks[index]->pending_lod.store(1);
            ASSERT_TRUE(world->adopt_streamed_chunk(patch.chunks[index]));
            patch.evidence.halo[index].epoch = patch.chunks[index]->near_terrain_epoch.load();
        }
        const auto snapshot = patch.Publish();
        ASSERT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
        if (removal < 2) {
            entt::registry registry;
            // Activation runs every four updates; no simulation or job threads.
            for (int frame = 0; frame < 4; ++frame) {
                world->update(registry, Vec3(1024, 0, 1024), nullptr, false);
                if (frame < 3) {
                    ASSERT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
                }
            }
        } else if (removal == 2) {
            world->clear_world(nullptr);
        } else {
            world.reset();
            world = std::make_unique<Systems::SHIELD_WorldSystem>(
                nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
        }
        ASSERT_EQ(world->find_streamed_chunk(IVec3(0)), nullptr);
        const auto replacement = std::make_shared<Chunk>(IVec3(0));
        ASSERT_TRUE(world->adopt_streamed_chunk(replacement));
        EXPECT_EQ(world->find_streamed_chunk(IVec3(0)), replacement);
        EXPECT_FALSE(snapshot->matches(*patch.chunks[13], 7));
        EXPECT_FALSE(snapshot->eligible(*patch.chunks[13], 7, 0));
    }
}

TEST(NearTerrainEligibility, EvidenceWithoutRefinementRetainsNoLattice) {
    EligibilityPatch patch;
    auto densities = MakePatch().densities;
    const auto expected = densities;
    const auto snapshot = World::CompleteNearTerrainMeshSnapshot(patch.evidence, densities);
    patch.chunks[13]->mesh_version.store(7);
    patch.chunks[13]->near_terrain_snapshot.store(snapshot);
    EXPECT_TRUE(snapshot->densities.empty());
    EXPECT_EQ(snapshot->densities.capacity(), 0u);
    EXPECT_EQ(densities, expected);
    EXPECT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
}

TEST(NearTerrainEligibility, TemporaryViewsPreserveResidentEvidence) {
    EligibilityPatch patch;
    const auto snapshot = patch.Publish();
    {
        WorldStreamingState view;
        for (const auto& chunk : patch.chunks) {
            ASSERT_TRUE(view.insert_chunk(chunk));
        }
    }
    EXPECT_TRUE(snapshot->eligible(*patch.chunks[13], 7, 0));
}

TEST(NearTerrainEligibility, RejectsStaleQueuedRecordsAndRetiredOwners) {
    for (int stale = 0; stale < 8; ++stale) {
        SCOPED_TRACE(stale);
        EligibilityPatch patch;
        const auto snapshot = patch.Publish();
        auto& chunk = *patch.chunks[13];
        if (stale == 0) {
            chunk.mesh_version.store(8);
        } else if (stale == 1) {
            chunk.near_terrain_snapshot.store({});
        } else if (stale == 2) {
            patch.chunks[0].reset();
        } else if (stale == 3) {
            patch.chunks[0]->normalize_for_park();
        } else if (stale == 4) {
            patch.chunks[0]->reset_pending_jobs();
        } else if (stale == 5) {
            patch.chunks[0]->near_terrain_epoch.store(std::numeric_limits<u64>::max());
            patch.chunks[0]->invalidate_near_terrain_snapshot();
        } else if (stale == 6) {
            chunk.mesh_version.store(0);
        } else {
            const auto replacement = patch.Publish();
            ASSERT_NE(replacement, snapshot);
        }
        EXPECT_FALSE(snapshot->matches(chunk, 7));
        EXPECT_FALSE(snapshot->eligible(chunk, 7, 0));
    }
    const Chunk missing(IVec3(0));
    EXPECT_EQ(World::CurrentNearTerrainMeshSnapshot(missing, 0), nullptr);
}

TEST(NearTerrainEligibility, RefusesMissingPartialEditedCoarseAndCaveEvidence) {
    for (int invalid = 0; invalid < 11; ++invalid) {
        SCOPED_TRACE(invalid);
        EligibilityPatch patch;
        auto densities = MakePatch().densities;
        if (invalid == 0) {
            densities.clear();
        } else if (invalid == 1) {
            densities.pop_back();
        } else if (invalid == 2) {
            densities.push_back(0);
        } else if (invalid == 3) {
            densities[8 + 8 * 17 + 8 * 17 * 17] = -1;
        } else if (invalid == 4) {
            densities[0] = std::numeric_limits<float>::quiet_NaN();
        } else if (invalid == 5) {
            patch.evidence.generated_current_params = false;
        } else if (invalid == 6) {
            patch.evidence.unedited_halo = false;
        } else if (invalid == 7) {
            patch.evidence.sample_step = 2;
        } else if (invalid == 8) {
            patch.evidence.sample_step = 0;
        } else if (invalid == 9) {
            patch.evidence.halo[0] = {};
        } else {
            patch.evidence.halo[0].epoch = std::numeric_limits<u64>::max();
        }
        for (bool retain_densities : {false, true}) {
            SCOPED_TRACE(retain_densities);
            const auto snapshot =
                World::CompleteNearTerrainMeshSnapshot(patch.evidence, densities, retain_densities);
            auto& chunk = *patch.chunks[13];
            chunk.mesh_version.store(7);
            chunk.near_terrain_snapshot.store(snapshot);
            EXPECT_FALSE(snapshot->eligible(chunk, 7, 0));
            EXPECT_LE(snapshot->densities.size(), World::kNearTerrainDensityCount);
            if (!retain_densities || invalid < 9) {
                EXPECT_TRUE(snapshot->densities.empty());
            }
        }
    }
}

namespace {
void ExpectIdenticalPayload(const NearTerrainRenderMesh& actual,
                            const NearTerrainRenderMesh& expected) {
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    ASSERT_EQ(actual.indices.size(), expected.indices.size());
    if (actual.vertices.empty()) {
        return;
    }
    EXPECT_EQ(std::memcmp(actual.vertices.data(),
                          expected.vertices.data(),
                          actual.vertices.size() * sizeof(VoxelVertex)),
              0);
    EXPECT_EQ(std::memcmp(actual.indices.data(),
                          expected.indices.data(),
                          actual.indices.size() * sizeof(u32)),
              0);
}

NearTerrainUploadResult Prepare(NearTerrainRefinementCache& cache,
                                const EligibilityPatch& patch,
                                NearTerrainRenderMesh& mesh,
                                double elapsed = 0) {
    const auto& chunk = *patch.chunks[13];
    return cache.prepare(chunk,
                         chunk.mesh_version.load(),
                         chunk.near_terrain_snapshot.load(),
                         0,
                         mesh.vertices,
                         mesh.indices,
                         elapsed);
}
} // namespace

TEST(NearTerrainUpload, DisabledPayloadIsByteIdenticalAndEnabledPayloadIsCached) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish();
    const auto original = MakePatch().mesh;
    auto payload = original;
    NearTerrainRefinementCache cache;
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    ExpectIdenticalPayload(payload, original);
    EXPECT_EQ(cache.builds(), 0u);
    EXPECT_TRUE(cache.set_enabled(true));
    ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
    EXPECT_GT(payload.indices.size(), original.indices.size());
    const auto refined = payload;
    NearTerrainUploadState resident;
    resident.set(evidence);
    EXPECT_TRUE(resident.matches(evidence));
    EXPECT_FALSE(resident.matches(nullptr));
    payload = original;
    ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
    ExpectIdenticalPayload(payload, refined);
    ExpectIdenticalPayload(original, MakePatch().mesh);
    EXPECT_EQ(cache.builds(), 1u);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.set_enabled(true));
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_TRUE(cache.set_enabled(false));
    EXPECT_EQ(cache.size(), 0u);
    payload = original;
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    ExpectIdenticalPayload(payload, original);
    EXPECT_TRUE(cache.set_enabled(true));
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
    EXPECT_EQ(cache.builds(), 2u);
}

TEST(NearTerrainUpload, VersionReplacementAndEveryHaloInvalidationDiscardCachedGeometry) {
    for (int change = 0; change < 30; ++change) {
        SCOPED_TRACE(change);
        EligibilityPatch patch;
        const auto evidence = patch.Publish();
        NearTerrainRefinementCache cache;
        cache.set_enabled(true);
        auto payload = MakePatch().mesh;
        ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
        NearTerrainUploadState resident;
        resident.set(evidence);
        if (change < 27) {
            patch.chunks[static_cast<std::size_t>(change)]->invalidate_near_terrain_snapshot();
        } else if (change == 27) {
            patch.chunks[13]->mesh_version.store(8);
        } else if (change == 28) {
            patch.chunks[13]->near_terrain_snapshot.store({});
        } else {
            const auto replacement = patch.Publish();
            EXPECT_FALSE(resident.matches(replacement));
        }
        payload = MakePatch().mesh;
        const auto current = cache.observe(*patch.chunks[13],
                                           patch.chunks[13]->mesh_version.load(),
                                           patch.chunks[13]->near_terrain_snapshot.load(),
                                           0);
        EXPECT_FALSE(resident.matches(current));
        EXPECT_EQ(cache.size(), 0u);
        const auto result = Prepare(cache, patch, payload);
        if (change == 29) {
            EXPECT_EQ(result, NearTerrainUploadResult::Refined);
            EXPECT_EQ(cache.builds(), 2u);
        } else {
            EXPECT_EQ(result, NearTerrainUploadResult::Unrefined);
            ExpectIdenticalPayload(payload, MakePatch().mesh);
            EXPECT_EQ(cache.builds(), 1u);
        }
    }
}

TEST(NearTerrainUpload, IneligibleAndKernelRefusalsPreservePayloadBytes) {
    for (int refusal = 0; refusal < 7; ++refusal) {
        SCOPED_TRACE(refusal);
        EligibilityPatch patch;
        if (refusal == 0) {
            patch.evidence.sample_step = 2;
        } else if (refusal == 1) {
            patch.evidence.unedited_halo = false;
        } else if (refusal == 2) {
            patch.evidence.generated_current_params = false;
        }
        const auto evidence = patch.Publish();
        auto original = MakePatch().mesh;
        if (refusal == 3) {
            original.indices[0] = std::numeric_limits<u32>::max();
        } else if (refusal == 4) {
            original.indices.push_back(0);
        } else if (refusal == 5) {
            original.vertices.resize(kNearTerrainMaxInputVertices + 1);
        }
        NearTerrainRefinementCache cache;
        cache.set_enabled(true);
        const float distance = refusal == 6 ? 1024.01f : 0;
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto payload = original;
            EXPECT_EQ(
                cache.prepare(
                    *patch.chunks[13], 7, evidence, distance, payload.vertices, payload.indices, 0),
                NearTerrainUploadResult::Unrefined);
            ExpectIdenticalPayload(payload, original);
        }
        EXPECT_EQ(cache.builds(), refusal >= 3 && refusal <= 5 ? 1u : 0u);
    }
}

TEST(NearTerrainUpload, CacheIsBoundedAndRetirementAndRangeChangesReleaseEntries) {
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    EligibilityPatch patch;
    std::vector<std::shared_ptr<Chunk>> owners;
    for (std::size_t index = 0; index <= NearTerrainRefinementCache::kCapacity; ++index) {
        auto owner = std::make_shared<Chunk>(IVec3(static_cast<int>(index), 0, 0));
        owners.push_back(owner);
        patch.chunks[13] = owner;
        patch.evidence.halo[13] = {owner, 0};
        const auto evidence = patch.Publish();
        auto payload = MakePatch().mesh;
        ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
        EXPECT_LE(cache.size(), NearTerrainRefinementCache::kCapacity);
    }
    EXPECT_EQ(cache.size(), NearTerrainRefinementCache::kCapacity);
    const auto& first = *owners.front();
    auto payload = MakePatch().mesh;
    EXPECT_EQ(
        cache.prepare(
            first, 7, first.near_terrain_snapshot.load(), 0, payload.vertices, payload.indices, 0),
        NearTerrainUploadResult::Refined);
    EXPECT_EQ(cache.builds(), NearTerrainRefinementCache::kCapacity + 2);
    const auto& last = *owners.back();
    EXPECT_EQ(cache.observe(last, 7, last.near_terrain_snapshot.load(), 1024.01f), nullptr);
    EXPECT_EQ(cache.size(), NearTerrainRefinementCache::kCapacity - 1);
    cache.forget(first.get_id());
    EXPECT_EQ(cache.size(), NearTerrainRefinementCache::kCapacity - 2);
    cache.forget(first.get_id());
    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
}

TEST(NearTerrainUpload, RefinedBytesAndDeadlineDeferWorkBeforeKernelExecution) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish();
    const auto original = MakePatch().mesh;
    std::size_t raw_bytes = 0;
    ASSERT_TRUE(mesh_upload_bytes(
        original.vertices.size(), sizeof(VoxelVertex), original.indices.size(), raw_bytes));
    const auto cost = NearTerrainRefinementCache::upload_cost(
        true, original.vertices.size(), original.indices.size(), raw_bytes);
    EXPECT_GT(cost, raw_bytes);
    EXPECT_EQ(NearTerrainRefinementCache::upload_cost(
                  false, original.vertices.size(), original.indices.size(), raw_bytes),
              raw_bytes);
    EXPECT_EQ(NearTerrainRefinementCache::upload_cost(
                  true, kNearTerrainMaxInputVertices + 1, original.indices.size(), raw_bytes),
              raw_bytes);
    EXPECT_EQ(
        NearTerrainRefinementCache::upload_cost(
            true, original.vertices.size(), std::numeric_limits<std::size_t>::max(), raw_bytes),
        raw_bytes);
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    for (const double elapsed : {3.0,
                                 4.0,
                                 -1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        auto payload = original;
        EXPECT_EQ(Prepare(cache, patch, payload, elapsed), NearTerrainUploadResult::Deferred);
        ExpectIdenticalPayload(payload, original);
        EXPECT_EQ(cache.builds(), 0u);
    }
    UploadQueue<int> queue;
    queue.refresh(1, 1);
    queue.refresh(2, 2);
    const UploadLimits limits{.bytes = cost, .count = 8, .progress = 1, .milliseconds = 3};
    const auto work = drain_uploads(
        queue,
        limits,
        [cost](int) { return cost; },
        [&](int) {
            auto payload = original;
            return Prepare(cache, patch, payload) == NearTerrainUploadResult::Refined;
        },
        [] { return 0.0; });
    EXPECT_EQ(work.attempts, 1u);
    EXPECT_EQ(work.bytes, cost);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(cache.builds(), 1u);
    EXPECT_EQ(work.byte_overshoot, 0u);
    const auto resumed = drain_uploads(
        queue,
        limits,
        [cost](int) { return cost; },
        [&](int) {
            auto payload = original;
            return Prepare(cache, patch, payload) == NearTerrainUploadResult::Refined;
        },
        [] { return 0.0; });
    EXPECT_EQ(resumed.adopted, 1u);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(cache.builds(), 1u);
}

TEST(NearTerrainUpload, LiveEligibilityCopiesItsLatticeWithoutMovingAuthority) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish(false);
    ASSERT_TRUE(evidence->densities.empty());
    ASSERT_TRUE(evidence->eligible(*patch.chunks[13], 7, 0));
    const auto original = MakePatch().mesh;
    const auto source = patch.chunks[13]->sdf_data;
    auto payload = original;
    NearTerrainRefinementCache cache;
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    ExpectIdenticalPayload(payload, original);
    cache.set_enabled(true);
    EXPECT_EQ(Prepare(cache, patch, payload, 3.0), NearTerrainUploadResult::Deferred);
    EXPECT_EQ(cache.builds(), 0u);
    EXPECT_EQ(patch.chunks[13]->sdf_data, source);
    ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
    EXPECT_GT(payload.indices.size(), original.indices.size());
    EXPECT_EQ(patch.chunks[13]->sdf_data, source);
    EXPECT_TRUE(evidence->densities.empty());
    EXPECT_EQ(cache.builds(), 1u);
    const auto refined = payload;
    payload = original;
    ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Refined);
    ExpectIdenticalPayload(payload, refined);
    EXPECT_EQ(cache.builds(), 1u);
    patch.chunks[13]->mark_sdf_loaded_or_edited();
    payload = original;
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    ExpectIdenticalPayload(payload, original);
    EXPECT_EQ(cache.size(), 0u);
}

TEST(NearTerrainUpload, LiveLatticeRefusesMissingPartialOversizedAndInvalidSamples) {
    for (int invalid = 0; invalid < 5; ++invalid) {
        SCOPED_TRACE(invalid);
        EligibilityPatch patch;
        const auto evidence = patch.Publish(false);
        auto& source = patch.chunks[13]->sdf_data;
        if (invalid == 0) {
            source.clear();
        } else if (invalid == 1) {
            source.pop_back();
        } else if (invalid == 2) {
            source.push_back(0);
        } else if (invalid == 3) {
            source[0] = std::numeric_limits<float>::infinity();
        } else {
            source[8 + 8 * 17 + 8 * 17 * 17] = -1;
        }
        const auto expected_source = source;
        const auto original = MakePatch().mesh;
        auto payload = original;
        NearTerrainRefinementCache cache;
        cache.set_enabled(true);
        EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
        ExpectIdenticalPayload(payload, original);
        EXPECT_EQ(source, expected_source);
        EXPECT_TRUE(evidence->densities.empty());
        EXPECT_EQ(cache.builds(), 1u);
        EXPECT_EQ(Prepare(cache, patch, payload, 3.0), NearTerrainUploadResult::Unrefined);
        ExpectIdenticalPayload(payload, original);
        EXPECT_EQ(cache.builds(), 1u);
    }
}

TEST(NearTerrainUpload, CachedRefusalsRemainImmediateAfterTheDeadline) {
    EligibilityPatch patch;
    patch.Publish();
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    auto original = MakePatch().mesh;
    original.indices.front() = std::numeric_limits<u32>::max();
    auto payload = original;
    ASSERT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    ASSERT_EQ(cache.builds(), 1u);
    for (const double elapsed : {3.0,
                                 4.0,
                                 -1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        SCOPED_TRACE(elapsed);
        EXPECT_EQ(Prepare(cache, patch, payload, elapsed), NearTerrainUploadResult::Unrefined);
        ExpectIdenticalPayload(payload, original);
        EXPECT_EQ(cache.builds(), 1u);
    }
    // An otherwise identical publication still invalidates the refusal key.
    patch.Publish();
    EXPECT_EQ(Prepare(cache, patch, payload, 3.0), NearTerrainUploadResult::Deferred);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.builds(), 1u);
    ExpectIdenticalPayload(payload, original);
    EXPECT_EQ(Prepare(cache, patch, payload), NearTerrainUploadResult::Unrefined);
    EXPECT_EQ(cache.builds(), 2u);
}

TEST(NearTerrainUpload, SaturatedDeadlineStillUploadsBaselineMeshes) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish();
    const auto original = MakePatch().mesh;
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    UploadQueue<int> baseline;
    UploadQueue<int> refinement;
    baseline.refresh(1, 1);
    baseline.refresh(2, 2);
    refinement.refresh(3, 3);
    NearTerrainUploadState resident;
    const auto cost = [](int) {
        return std::size_t{1};
    };
    const auto adopt = [&](int item) {
        auto payload = original;
        const auto result = Prepare(cache, patch, payload, 3.0);
        EXPECT_EQ(result, NearTerrainUploadResult::Deferred);
        ExpectIdenticalPayload(payload, original);
        resident.set(result == NearTerrainUploadResult::Deferred ? nullptr : evidence);
        EXPECT_FALSE(resident.matches(evidence));
        EXPECT_LT(item, 3);
        return true;
    };
    for (std::size_t frame = 0; frame < 2; ++frame) {
        const auto work =
            drain_terrain_uploads(baseline, refinement, cost, adopt, [] { return 3.0; });
        EXPECT_EQ(work.attempts, 1u);
        EXPECT_EQ(work.adopted, 1u);
        EXPECT_EQ(baseline.size(), 1u - frame);
        EXPECT_EQ(refinement.size(), 1u);
        EXPECT_EQ(cache.builds(), 0u);
    }
    const auto resumed = drain_terrain_uploads(
        baseline,
        refinement,
        cost,
        [&](int item) {
            EXPECT_EQ(item, 3);
            auto payload = original;
            const auto result = Prepare(cache, patch, payload);
            EXPECT_EQ(result, NearTerrainUploadResult::Refined);
            EXPECT_GT(payload.indices.size(), original.indices.size());
            resident.set(evidence);
            return result == NearTerrainUploadResult::Refined;
        },
        [] { return 0.0; });
    EXPECT_EQ(resumed.adopted, 1u);
    EXPECT_EQ(refinement.size(), 0u);
    EXPECT_TRUE(resident.matches(evidence));
    EXPECT_EQ(cache.builds(), 1u);
}

TEST(NearTerrainUpload, OptionalReplacementsShareLimitsAndNeverClaimProgressAllowance) {
    for (const double elapsed : {3.0,
                                 -1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        SCOPED_TRACE(elapsed);
        UploadQueue<int> baseline;
        UploadQueue<int> refinement;
        refinement.refresh(1, 1);
        const auto work = drain_terrain_uploads(
            baseline,
            refinement,
            [](int) { return std::size_t{1}; },
            [](int) {
                ADD_FAILURE();
                return true;
            },
            [elapsed] { return elapsed; });
        EXPECT_EQ(work.attempts, 0u);
        EXPECT_EQ(refinement.size(), 1u);
    }
    for (int limit = 0; limit < 4; ++limit) {
        SCOPED_TRACE(limit);
        UploadQueue<int> baseline;
        UploadQueue<int> refinement;
        const auto baseline_count = limit == 1 ? kTerrainUploadLimits.count : std::size_t{1};
        if (limit < 2) {
            for (std::size_t index = 0; index < baseline_count; ++index) {
                baseline.refresh(index, 0);
            }
        }
        if (limit != 3) {
            refinement.refresh(9, 1);
        }
        const auto work = drain_terrain_uploads(
            baseline,
            refinement,
            [limit](int item) {
                return limit == 0 || (limit == 2 && item == 1)
                           ? kTerrainUploadLimits.bytes + (limit == 2 ? 1u : 0u)
                           : std::size_t{1};
            },
            [](int item) {
                EXPECT_EQ(item, 0);
                return true;
            },
            [] { return 0.0; });
        EXPECT_EQ(work.attempts, limit < 2 ? baseline_count : 0u);
        EXPECT_EQ(refinement.size(), limit == 3 ? 0u : 1u);
    }
}

TEST(NearTerrainUpload, DeadlineAfterOptionalCopyPreservesResidentAndRetries) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish();
    const auto original = MakePatch().mesh;
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    NearTerrainUploadState resident;
    UploadQueue<int> baseline;
    UploadQueue<int> refinement;
    refinement.refresh(1, 1);
    const auto work = drain_terrain_uploads(
        baseline,
        refinement,
        [](int) { return std::size_t{1}; },
        [&](int) {
            auto payload = original;
            const auto result = Prepare(cache, patch, payload, 3.0);
            EXPECT_EQ(result, NearTerrainUploadResult::Deferred);
            ExpectIdenticalPayload(payload, original);
            return result != NearTerrainUploadResult::Deferred;
        },
        [] { return 0.0; });
    EXPECT_EQ(work.attempts, 1u);
    EXPECT_EQ(work.adopted, 0u);
    EXPECT_EQ(refinement.size(), 1u);
    EXPECT_EQ(cache.builds(), 0u);
    EXPECT_TRUE(resident.matches(nullptr));
    EXPECT_FALSE(resident.matches(evidence));
}

TEST(NearTerrainUpload, OptionalFailureRotatesWithoutRepeatingAndCanResume) {
    UploadQueue<int> baseline;
    UploadQueue<int> refinement;
    refinement.refresh(1, 1);
    refinement.refresh(2, 2);
    int calls = 0;
    const auto cost = [](int) {
        return std::size_t{1};
    };
    const auto failed = drain_terrain_uploads(
        baseline,
        refinement,
        cost,
        [&](int) {
            ++calls;
            return false;
        },
        [] { return 0.0; });
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(failed.attempts, 2u);
    EXPECT_EQ(failed.adopted, 0u);
    EXPECT_EQ(refinement.size(), 2u);
    const auto resumed = drain_terrain_uploads(
        baseline, refinement, cost, [](int) { return true; }, [] { return 0.0; });
    EXPECT_EQ(resumed.adopted, 2u);
    EXPECT_EQ(refinement.size(), 0u);
}

TEST(NearTerrainUpload, SettingDefaultsAndPersistenceRoundTrip) {
    using namespace luminumbra::core;
    ResolvedGraphics resolved;
    std::string error;
    for (const auto preset : {GraphicsPreset::Low,
                              GraphicsPreset::Medium,
                              GraphicsPreset::High,
                              GraphicsPreset::Ultra}) {
        for (u32 revision = 1; revision <= kGraphicsPresetRevision; ++revision) {
            GraphicsSettings settings;
            settings.base_preset = preset;
            settings.preset_revision = revision;
            ASSERT_TRUE(ResolveGraphics(settings, {}, resolved, error));
            EXPECT_FALSE(resolved.values.terrain_near_refinement);
        }
    }
    const Luminumbra::TestSupport::ScopedTempFile file("terrain_graphics_", ".json");
    auto config = SystemConfig::Defaults();
    for (const bool enabled : {true, false}) {
        config.user().graphics.overrides["terrain_near_refinement"] = enabled;
        ASSERT_TRUE(config.SaveUserOverlay(file.Path().string()));
        const auto reloaded = SystemConfig::LoadLayered("", file.Path().string());
        ASSERT_TRUE(ResolveGraphics(reloaded.user().graphics, {}, resolved, error));
        EXPECT_EQ(resolved.values.terrain_near_refinement, enabled);
        EXPECT_EQ(reloaded.user().graphics.overrides, config.user().graphics.overrides);
    }
    const auto previous = resolved.values;
    config.user().graphics.overrides["terrain_near_refinement"] = 1.0;
    EXPECT_FALSE(ResolveGraphics(config.user().graphics, {}, resolved, error));
    EXPECT_EQ(resolved.values, previous);
    EXPECT_FALSE(config.SaveUserOverlay(file.Path().string()));
}

TEST(NearTerrainUpload, EmptyMissingAndInvalidDistanceInputsRemainUnrefined) {
    EligibilityPatch patch;
    const auto evidence = patch.Publish();
    NearTerrainRefinementCache cache;
    cache.set_enabled(true);
    const auto original = MakePatch().mesh;
    auto payload = original;
    EXPECT_EQ(cache.prepare(*patch.chunks[13], 7, nullptr, 0, payload.vertices, payload.indices, 0),
              NearTerrainUploadResult::Unrefined);
    ExpectIdenticalPayload(payload, original);
    for (float distance :
         {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_EQ(
            cache.prepare(
                *patch.chunks[13], 7, evidence, distance, payload.vertices, payload.indices, 0),
            NearTerrainUploadResult::Unrefined);
        ExpectIdenticalPayload(payload, original);
    }
    NearTerrainRenderMesh empty;
    EXPECT_EQ(Prepare(cache, patch, empty), NearTerrainUploadResult::Unrefined);
    EXPECT_TRUE(empty.vertices.empty());
    EXPECT_TRUE(empty.indices.empty());
    EXPECT_EQ(cache.builds(), 1u);
}

TEST(NearTerrainUpload, ExpiredEvidenceCannotMatchDisabledResidentState) {
    NearTerrainUploadState resident;
    {
        EligibilityPatch patch;
        const auto evidence = patch.Publish();
        resident.set(evidence);
        EXPECT_TRUE(resident.matches(evidence));
    }
    EXPECT_TRUE(resident.evidence.expired());
    EXPECT_FALSE(resident.matches(nullptr));
    resident.set(nullptr);
    EXPECT_TRUE(resident.matches(nullptr));
}
