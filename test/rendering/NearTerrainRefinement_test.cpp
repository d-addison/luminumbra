#include "luminumbra_client/rendering/NearTerrainRefinement.h"
#include "luminumbra_common/world/NearTerrainMeshSnapshot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

    std::shared_ptr<const World::NearTerrainMeshSnapshot> Publish() {
        auto densities = MakePatch().densities;
        const auto snapshot = World::CompleteNearTerrainMeshSnapshot(evidence, densities);
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
    const auto snapshot = World::CompleteNearTerrainMeshSnapshot(patch.evidence, densities);
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
        const auto snapshot = World::CompleteNearTerrainMeshSnapshot(patch.evidence, densities);
        auto& chunk = *patch.chunks[13];
        chunk.mesh_version.store(7);
        chunk.near_terrain_snapshot.store(snapshot);
        EXPECT_FALSE(snapshot->eligible(chunk, 7, 0));
        EXPECT_LE(snapshot->densities.size(), World::kNearTerrainDensityCount);
    }
}
