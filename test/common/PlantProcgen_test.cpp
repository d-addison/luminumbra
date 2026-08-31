// rendering — deterministic procedural plant geometry. contract tests from spec
// ..005. Headlessly verifiable: the
// generator is a pure, libm-free, deterministic function of (genome, stage).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <glm/glm.hpp>

#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/systems/PlantProcgen.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::foliage::GeneratePlantMesh;

Comp::PlantGenomeComponent UniformGenome(float v) {
    Comp::PlantGenomeComponent g;
    for (auto& gene : g.genes)
        gene = v;
    return g;
}

std::uint64_t HashMesh(const std::vector<glm::vec3>& mesh) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a 64 offset basis
    for (const glm::vec3& vert : mesh) {
        const float comps[3] = {vert.x, vert.y, vert.z};
        for (float f : comps) {
            std::uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int b = 0; b < 4; ++b) {
                h ^= (bits >> (b * 8)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

const std::uint8_t kFruiting = static_cast<std::uint8_t>(Comp::PlantStage::Fruiting);

// pure/deterministic: identical input -> identical bytes.
TEST(PlantProcgen, DeterministicAcrossCalls) {
    const Comp::PlantGenomeComponent g = UniformGenome(0.3f);
    const auto a = GeneratePlantMesh(g, kFruiting);
    const auto b = GeneratePlantMesh(g, kFruiting);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].x, b[i].x);
        EXPECT_EQ(a[i].y, b[i].y);
        EXPECT_EQ(a[i].z, b[i].z);
    }
}

// non-empty + bounded at the mature stage.
TEST(PlantProcgen, NonEmptyAndBounded) {
    const auto m = GeneratePlantMesh(UniformGenome(0.8f), kFruiting);
    EXPECT_GT(m.size(), 0u);
    EXPECT_LE(m.size(), 4096u);
    EXPECT_EQ(m.size() % 2u, 0u); // line list -> pairs of endpoints
}

// geometry complexity is non-decreasing in growth stage.
TEST(PlantProcgen, MaturationMonotonic) {
    const Comp::PlantGenomeComponent g = UniformGenome(0.5f);
    std::size_t prev = 0;
    for (std::uint8_t s = 0; s < static_cast<std::uint8_t>(Comp::PlantStage::Count); ++s) {
        const std::size_t n = GeneratePlantMesh(g, s).size();
        EXPECT_GE(n, prev) << "stage " << int(s) << " has fewer verts than the prior stage";
        prev = n;
    }
}

// different genomes produce different geometry.
TEST(PlantProcgen, GeneticVariation) {
    const auto a = GeneratePlantMesh(UniformGenome(0.2f), kFruiting);
    const auto b = GeneratePlantMesh(UniformGenome(0.9f), kFruiting);
    EXPECT_NE(HashMesh(a), HashMesh(b));
}

// ATMOSPHERIC: phototropism bends the plant toward the sun, so a leaning
// sun + nonzero phototropism produces different geometry than the pure genetic form —
// and the response is itself deterministic.
TEST(PlantProcgen, PhototropismBendsTowardSun) {
    using luminumbra::foliage::PlantEnvDir;
    const Comp::PlantGenomeComponent g = UniformGenome(0.7f); // leafy -> seeks light

    const auto baseline = GeneratePlantMesh(g, kFruiting); // default env: sun up, no photo
    PlantEnvDir leaning;
    leaning.sun_dir = glm::vec3(0.7071f, 0.7071f, 0.0f); // sun low in the +x sky
    leaning.phototropism = 0.6f;
    const auto bent = GeneratePlantMesh(g, kFruiting, leaning);
    const auto bent2 = GeneratePlantMesh(g, kFruiting, leaning);

    EXPECT_NE(HashMesh(baseline), HashMesh(bent)); // atmosphere changed the shape
    EXPECT_EQ(HashMesh(bent), HashMesh(bent2));    //...deterministically

    // The canopy should shift toward the sun: mean X of the bent plant > baseline.
    auto meanX = [](const std::vector<glm::vec3>& m) {
        double s = 0.0;
        for (const auto& v : m)
            s += v.x;
        return m.empty() ? 0.0 : s / static_cast<double>(m.size());
    };
    EXPECT_GT(meanX(bent), meanX(baseline));
}

// cross-platform determinism pinned to a golden FNV-1a value. A change
// to the libm-free math or the algorithm is a DELIBERATE move of this literal.
TEST(PlantProcgen, CrossPlatformGolden) {
    const auto m = GeneratePlantMesh(UniformGenome(0.3f), kFruiting);
    EXPECT_EQ(HashMesh(m), 10683496572422438044ull); // libm-free determinism golden
}

// --- Rich structure (pipe-model branches + sun-facing leaves) ---

using luminumbra::foliage::GeneratePlant;
using luminumbra::foliage::PlantStructure;

std::uint64_t HashStructure(const PlantStructure& s) {
    std::vector<glm::vec3> flat;
    for (const auto& br : s.branches) {
        flat.push_back(br.a);
        flat.push_back(br.b);
        flat.push_back(glm::vec3(br.radius, static_cast<float>(br.depth), 0.0f));
    }
    for (const auto& lf : s.leaves) {
        flat.push_back(lf.pos);
        flat.push_back(lf.normal);
    }
    return HashMesh(flat);
}

// Pipe-model: the trunk (depth 0) is the thickest segment.
TEST(PlantProcgen, PipeModelTrunkThickest) {
    const PlantStructure s = GeneratePlant(UniformGenome(0.6f), kFruiting);
    ASSERT_FALSE(s.branches.empty());
    float trunk_r = -1.0f, max_r = 0.0f;
    for (const auto& br : s.branches) {
        max_r = std::max(max_r, br.radius);
        if (br.depth == 0)
            trunk_r = br.radius;
    }
    EXPECT_GT(trunk_r, 0.0f);
    EXPECT_FLOAT_EQ(trunk_r, max_r); // root carries every leaf's cross-section -> thickest
}

// Leaves grow with maturity, and a leafier genome grows more of them.
TEST(PlantProcgen, LeafCountScalesWithStageAndGenome) {
    const Comp::PlantGenomeComponent g = UniformGenome(0.6f);
    std::size_t prev = 0;
    for (std::uint8_t st = 0; st < static_cast<std::uint8_t>(Comp::PlantStage::Count); ++st) {
        const std::size_t n = GeneratePlant(g, st).leaves.size();
        EXPECT_GE(n, prev);
        prev = n;
    }
    const std::size_t leafy = GeneratePlant(UniformGenome(0.7f), kFruiting).leaves.size();
    const std::size_t sparse = GeneratePlant(UniformGenome(0.3f), kFruiting).leaves.size();
    EXPECT_GT(leafy, sparse);
}

// ATMOSPHERIC: the whole plant (trunk included) leans toward the sun.
TEST(PlantProcgen, TrunkLeansTowardSun) {
    using luminumbra::foliage::PlantEnvDir;
    const Comp::PlantGenomeComponent g = UniformGenome(0.6f);

    auto trunk_tip_x = [](const PlantStructure& s) {
        for (const auto& br : s.branches)
            if (br.depth == 0)
                return br.b.x;
        return 0.0f;
    };
    const float straight = trunk_tip_x(GeneratePlant(g, kFruiting)); // sun up -> no lean
    PlantEnvDir leaning;
    leaning.sun_dir = glm::vec3(0.7071f, 0.7071f, 0.0f);
    leaning.phototropism = 0.6f;
    EXPECT_NEAR(straight, 0.0f, 1e-4f);
    EXPECT_GT(trunk_tip_x(GeneratePlant(g, kFruiting, leaning)), 0.05f); // leans toward +x sun
}

//  a hardy genome grows a narrow CONIFER; otherwise a wide BROADLEAF.
TEST(PlantProcgen, ConiferNarrowerThanBroadleaf) {
    Comp::PlantGenomeComponent conifer = UniformGenome(0.5f);
    conifer.genes[static_cast<std::size_t>(Comp::PlantGene::Hardiness)] = 0.8f; // hardy -> conifer
    Comp::PlantGenomeComponent broad = UniformGenome(0.5f);
    broad.genes[static_cast<std::size_t>(Comp::PlantGene::Hardiness)] = 0.2f; // -> broadleaf
    auto horizExtent = [](const PlantStructure& s) {
        float mx = 0.0f;
        for (const auto& b : s.branches) {
            mx = std::max(mx, std::abs(b.b.x));
            mx = std::max(mx, std::abs(b.b.z));
        }
        return mx;
    };
    EXPECT_LT(horizExtent(GeneratePlant(conifer, kFruiting)),
              horizExtent(GeneratePlant(broad, kFruiting)));
}

TEST(PlantProcgen, StructureGolden) {
    const std::uint64_t h = HashStructure(GeneratePlant(UniformGenome(0.6f), kFruiting));
    EXPECT_EQ(h, 6743362585011370933ull); // libm-free determinism golden (branches + leaf clusters)
}

// --- Tessellation: structure -> renderable triangle mesh ---

using luminumbra::foliage::ProcMesh;
using luminumbra::foliage::TessellatePlant;

std::uint64_t HashProcMesh(const ProcMesh& m) {
    std::vector<glm::vec3> flat;
    for (const auto& vtx : m.vertices) {
        flat.push_back(vtx.pos);
        flat.push_back(vtx.normal);
        flat.push_back(glm::vec3(vtx.uv.x, vtx.uv.y, 0.0f));
    }
    for (std::uint32_t idx : m.indices)
        flat.push_back(glm::vec3(static_cast<float>(idx), 0.0f, 0.0f));
    return HashMesh(flat);
}

TEST(PlantProcgen, TessellateValidAndCounts) {
    const int radial = 6;
    const auto s = GeneratePlant(UniformGenome(0.6f), kFruiting);
    const ProcMesh m = TessellatePlant(s, radial);

    // Every index references a real vertex; index buffer is whole triangles.
    EXPECT_FALSE(m.vertices.empty());
    EXPECT_EQ(m.indices.size() % 3u, 0u);
    for (std::uint32_t idx : m.indices)
        EXPECT_LT(idx, m.vertices.size());

    // Counts: each branch -> radial*2 verts + radial*6 indices; each leaf -> 4 verts + 6 indices.
    const std::size_t nb = s.branches.size(), nl = s.leaves.size();
    EXPECT_EQ(m.vertices.size(), nb * radial * 2u + nl * 4u);
    EXPECT_EQ(m.indices.size(), nb * radial * 6u + nl * 6u);
}

TEST(PlantProcgen, TessellateDeterministic) {
    const auto s = GeneratePlant(UniformGenome(0.6f), kFruiting);
    EXPECT_EQ(HashProcMesh(TessellatePlant(s)), HashProcMesh(TessellatePlant(s)));
}

TEST(PlantProcgen, TessellateGolden) {
    const auto s = GeneratePlant(UniformGenome(0.6f), kFruiting);
    EXPECT_EQ(HashProcMesh(TessellatePlant(s, 6)), 4738719477643067794ull); // tessellation golden
}

} // namespace
