// Pillar A — deterministic procedural plant geometry. RED-first tests from spec
// .forge/specs/foliage-polish/spec.md (AC-A-001..005). Headlessly verifiable: the
// generator is a pure, libm-free, deterministic function of (genome, stage).
#include <gtest/gtest.h>

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
    for (auto& gene : g.genes) gene = v;
    return g;
}

std::uint64_t HashMesh(const std::vector<glm::vec3>& mesh) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a 64 offset basis
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

// AC-A-001 — pure/deterministic: identical input -> identical bytes.
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

// AC-A-002 — non-empty + bounded at the mature stage.
TEST(PlantProcgen, NonEmptyAndBounded) {
    const auto m = GeneratePlantMesh(UniformGenome(0.8f), kFruiting);
    EXPECT_GT(m.size(), 0u);
    EXPECT_LE(m.size(), 4096u);
    EXPECT_EQ(m.size() % 2u, 0u);  // line list -> pairs of endpoints
}

// AC-A-003 — geometry complexity is non-decreasing in growth stage.
TEST(PlantProcgen, MaturationMonotonic) {
    const Comp::PlantGenomeComponent g = UniformGenome(0.5f);
    std::size_t prev = 0;
    for (std::uint8_t s = 0; s < static_cast<std::uint8_t>(Comp::PlantStage::Count); ++s) {
        const std::size_t n = GeneratePlantMesh(g, s).size();
        EXPECT_GE(n, prev) << "stage " << int(s) << " has fewer verts than the prior stage";
        prev = n;
    }
}

// AC-A-004 — different genomes produce different geometry.
TEST(PlantProcgen, GeneticVariation) {
    const auto a = GeneratePlantMesh(UniformGenome(0.2f), kFruiting);
    const auto b = GeneratePlantMesh(UniformGenome(0.9f), kFruiting);
    EXPECT_NE(HashMesh(a), HashMesh(b));
}

// AC-A-006 — ATMOSPHERIC: phototropism bends the plant toward the sun, so a leaning
// sun + nonzero phototropism produces different geometry than the pure genetic form —
// and the response is itself deterministic.
TEST(PlantProcgen, PhototropismBendsTowardSun) {
    using luminumbra::foliage::PlantEnvDir;
    const Comp::PlantGenomeComponent g = UniformGenome(0.7f);  // leafy -> seeks light

    const auto baseline = GeneratePlantMesh(g, kFruiting);  // default env: sun up, no photo
    PlantEnvDir leaning;
    leaning.sun_dir = glm::vec3(0.7071f, 0.7071f, 0.0f);  // sun low in the +x sky
    leaning.phototropism = 0.6f;
    const auto bent = GeneratePlantMesh(g, kFruiting, leaning);
    const auto bent2 = GeneratePlantMesh(g, kFruiting, leaning);

    EXPECT_NE(HashMesh(baseline), HashMesh(bent));  // atmosphere changed the shape
    EXPECT_EQ(HashMesh(bent), HashMesh(bent2));      // ...deterministically

    // The canopy should shift toward the sun: mean X of the bent plant > baseline.
    auto meanX = [](const std::vector<glm::vec3>& m) {
        double s = 0.0;
        for (const auto& v : m) s += v.x;
        return m.empty() ? 0.0 : s / static_cast<double>(m.size());
    };
    EXPECT_GT(meanX(bent), meanX(baseline));
}

// AC-A-005 — cross-platform determinism pinned to a golden FNV-1a value. A change
// to the libm-free math or the algorithm is a DELIBERATE move of this literal.
TEST(PlantProcgen, CrossPlatformGolden) {
    const auto m = GeneratePlantMesh(UniformGenome(0.3f), kFruiting);
    EXPECT_EQ(HashMesh(m), 4793091320348283183ull);  // libm-free determinism golden
}

}  // namespace
