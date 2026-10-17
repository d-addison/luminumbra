// CPU-only placement and real consume regression coverage. Synthetic terrain
// drives an independent legacy placement oracle; a frozen legacy consumer
// supplies registry, animation and physics side-effect expectations without GL.

#include "WorldDressing.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/AlarmComponents.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/components/PackHunterComponents.h"
#include "luminumbra_common/components/ScavengerComponent.h"
#include "luminumbra_common/components/TerritoryComponents.h"
#include "luminumbra_common/components/ThirstComponents.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/ThirstComponents.h"
#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/systems/CreatureProcgen.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

using Luminumbra::Client::BushPlacement;
using Luminumbra::Client::ComputeWorldDressing;
using Luminumbra::Client::CreaturePlacement;
using Luminumbra::Client::RockPlacement;
using Luminumbra::Client::TreePlacement;
using Luminumbra::Client::WorldDressingCallbacks;
using Luminumbra::Client::WorldDressingParams;
using Luminumbra::Client::WorldDressingResult;

// FNV-1a over the bytes of each FIELD in declaration order (never over whole
// structs — padding bytes are indeterminate and would make the hash lie).
struct FnvHasher {
    std::uint64_t h = 1469598103934665603ull;
    std::string emitted;
    void bytes(const void* p, std::size_t n) {
        emitted.append(static_cast<const char*>(p), n);
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template<typename T>
    void field(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "hash fields must be POD");
        bytes(&v, sizeof(v));
    }
    void vec3(const glm::vec3& v) {
        field(v.x);
        field(v.y);
        field(v.z);
    }
    void quat(const glm::quat& q) {
        field(q.x);
        field(q.y);
        field(q.z);
        field(q.w);
    }
};

FnvHasher PlacementFields(const WorldDressingResult& r) {
    FnvHasher f;
    for (const TreePlacement& t : r.trees) {
        f.vec3(t.position);
        f.quat(t.rotation);
        f.field(t.eff_scale);
        f.field(t.palette_index);
    }
    for (const RockPlacement& k : r.rocks) {
        f.vec3(k.position);
        f.vec3(k.scale);
        f.quat(k.rotation);
        f.field(k.palette_index);
    }
    for (const BushPlacement& b : r.bushes) {
        f.vec3(b.position);
        f.vec3(b.scale);
        f.quat(b.rotation);
        f.field(b.palette_index);
    }
    for (const CreaturePlacement& c : r.wildlife) {
        f.field(c.kind);
        f.vec3(c.position);
        f.field(c.yaw);
        f.field(c.species_index);
        f.field(c.size);
        f.vec3(c.build_scale);
        f.field(c.thirst);
        f.field(c.anim_phase);
        f.field(c.female);
    }
    return f;
}

std::uint64_t HashPlacements(const WorldDressingResult& r) {
    return PlacementFields(r).h;
}

std::uint64_t HashBushes(const WorldDressingResult& r) {
    FnvHasher f;
    for (const BushPlacement& b : r.bushes) {
        f.vec3(b.position);
        f.vec3(b.scale);
        f.quat(b.rotation);
        f.field(b.palette_index);
    }
    return f.h;
}

std::uint64_t HashTrees(const WorldDressingResult& r) {
    FnvHasher f;
    for (const TreePlacement& t : r.trees) {
        f.vec3(t.position);
        f.quat(t.rotation);
        f.field(t.eff_scale);
        f.field(t.palette_index);
    }
    return f.h;
}

// Synthetic world: gently rolling terrain well above water, all-air SDF (no
// roofed-cave rejects), lush vegetation, one biome. Pure functions of (x, z),
// so re-running the computation exercises only its own RNG/iteration order.
WorldDressingCallbacks SyntheticCallbacks(float terrain_base = 10.0f) {
    WorldDressingCallbacks cbs;
    cbs.terrain_height = [terrain_base](float x, float z) {
        return terrain_base + std::sin(x * 0.05f) * 3.0f + std::cos(z * 0.07f) * 2.0f;
    };
    cbs.water_level = [](float, float) {
        return 0.0f;
    };
    cbs.density_at = [](float, float, float) {
        return 1.0f;
    }; // air everywhere
    cbs.vegetation_density = [](float, float) {
        return 0.6f;
    };
    cbs.biome_name = [](float, float) {
        return std::string("meadow");
    };
    cbs.species_for_biome = [](const std::string&, std::size_t pick) {
        return static_cast<int>(pick % 5); // deterministic 5-species roster
    };
    return cbs;
}

WorldDressingParams SyntheticParams() {
    WorldDressingParams p;
    p.anchor_x = 100.0f;
    p.anchor_z = -40.0f;
    p.tree_palette_count = 4;
    p.rock_palette_count = 3;
    p.bush_palette_count = 5;
    p.compute_wildlife = true;
    p.herd_count = 12;
    return p;
}

} // namespace

// Same params + same callbacks twice -> byte-identical placement stream (hash
// AND field-by-field), non-empty in every lane, order stable by construction
// (the hash covers the stream in vector order, so any reorder changes it).
TEST(WorldDressing, PlacementDeterministicAndOrderStable) {
    const WorldDressingParams params = SyntheticParams();
    const WorldDressingCallbacks cbs = SyntheticCallbacks();

    const WorldDressingResult a = ComputeWorldDressing(params, cbs);
    const WorldDressingResult b = ComputeWorldDressing(params, cbs);

    // Every lane produced placements on the synthetic (all-valid) terrain.
    EXPECT_GT(a.trees.size(), 0u);
    EXPECT_GT(a.rocks.size(), 0u);
    EXPECT_GT(a.bushes.size(), 0u);
    EXPECT_GT(a.wildlife.size(), 0u);

    ASSERT_EQ(a.trees.size(), b.trees.size());
    ASSERT_EQ(a.rocks.size(), b.rocks.size());
    ASSERT_EQ(a.bushes.size(), b.bushes.size());
    ASSERT_EQ(a.wildlife.size(), b.wildlife.size());

    // The FNV field-stream hash is the cross-run fingerprint.
    EXPECT_EQ(HashPlacements(a), HashPlacements(b));

    // Belt-and-braces: exact field equality, element by element, in order.
    bool equal = true;
    for (std::size_t i = 0; i < a.trees.size() && equal; ++i) {
        const auto& x = a.trees[i];
        const auto& y = b.trees[i];
        equal = x.position == y.position && x.rotation == y.rotation &&
                x.eff_scale == y.eff_scale && x.palette_index == y.palette_index;
    }
    for (std::size_t i = 0; i < a.wildlife.size() && equal; ++i) {
        const auto& x = a.wildlife[i];
        const auto& y = b.wildlife[i];
        equal = x.kind == y.kind && x.position == y.position && x.yaw == y.yaw &&
                x.species_index == y.species_index && x.size == y.size &&
                x.build_scale == y.build_scale && x.thirst == y.thirst &&
                x.anim_phase == y.anim_phase && x.female == y.female;
    }
    EXPECT_TRUE(equal);

    // Palette indices stay inside the palettes the params pinned.
    for (const TreePlacement& t : a.trees) {
        ASSERT_GE(t.palette_index, 0);
        ASSERT_LT(t.palette_index, params.tree_palette_count);
    }
}

// The wildlife loop: deterministic across runs, creature entries carry resolved
// roster indices, and on a fully flooded terrain the loop emits ONLY the capped
// drinking-spot water holes (max 3) and zero creatures — the legacy water-cell
// `continue` behavior.
TEST(WorldDressing, WildlifeDeterministicAndWaterHolesCapped) {
    const WorldDressingParams params = SyntheticParams();

    {
        const WorldDressingCallbacks cbs = SyntheticCallbacks();
        const WorldDressingResult a = ComputeWorldDressing(params, cbs);
        const WorldDressingResult b = ComputeWorldDressing(params, cbs);
        std::size_t creatures = 0;
        for (const CreaturePlacement& c : a.wildlife) {
            if (c.kind != CreaturePlacement::Kind::Creature)
                continue;
            ++creatures;
            EXPECT_GE(c.species_index, 0);
            EXPECT_LT(c.species_index, 5);
        }
        EXPECT_GT(creatures, 0u);
        EXPECT_LE(a.wildlife.size(), static_cast<std::size_t>(params.herd_count));
        EXPECT_EQ(HashPlacements(a), HashPlacements(b));
    }

    {
        // Sink the terrain below the water line: every candidate is a water
        // cell -> at most 3 water holes, no creatures.
        WorldDressingCallbacks flooded = SyntheticCallbacks(-20.0f);
        const WorldDressingResult r = ComputeWorldDressing(params, flooded);
        std::size_t holes = 0, creatures = 0;
        for (const CreaturePlacement& c : r.wildlife) {
            if (c.kind == CreaturePlacement::Kind::WaterHole)
                ++holes;
            else
                ++creatures;
        }
        EXPECT_EQ(creatures, 0u);
        EXPECT_LE(holes, 3u);
        EXPECT_GT(holes, 0u);
    }
}

// THE load-bearing constraint: the three scatter loops share one sequential RNG
// stream. Disabling the rock palette (which skips the entire rock loop and its
// draws, as the legacy inline gate did) must leave the TREES identical (they
// draw first) but SHIFT the bushes (they draw after the rocks). If this ever
// fails in the "trees differ" direction, the stream seeding broke; if bushes
// stop shifting, someone split the stream per-loop and the layout no longer
// matches the legacy inline code.
TEST(WorldDressing, ScatterStreamSharedAcrossLayers) {
    const WorldDressingCallbacks cbs = SyntheticCallbacks();
    WorldDressingParams with_rocks = SyntheticParams();
    WorldDressingParams no_rocks = SyntheticParams();
    no_rocks.rock_palette_count = 0;

    const WorldDressingResult a = ComputeWorldDressing(with_rocks, cbs);
    const WorldDressingResult b = ComputeWorldDressing(no_rocks, cbs);

    EXPECT_EQ(HashTrees(a), HashTrees(b)); // trees draw before rocks: unchanged
    EXPECT_TRUE(b.rocks.empty());          // legacy gate: empty palette = no loop
    ASSERT_GT(a.bushes.size(), 0u);
    ASSERT_GT(b.bushes.size(), 0u);
    EXPECT_NE(HashBushes(a), HashBushes(b)); // bushes continue the stream after rocks
}

namespace LegacyDressingGolden {
using namespace Luminumbra::Client;

namespace {

float ScatterFrand(std::uint64_t& rng) {
    rng += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
}

} // namespace

std::uint64_t InitialScatterRngState(float anchor_x, float anchor_z) {
    return 0x9E3779B97F4A7C15ull ^
           (static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor_x)) *
            0xBF58476D1CE4E5B9ull) ^
           (static_cast<std::uint64_t>(static_cast<std::int64_t>(anchor_z)) *
            0x94D049BB133111EBull);
}

std::vector<TreePlacement> ComputeTreePlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng) {
    std::vector<TreePlacement> out;
    auto frand = [&rng]() {
        return ScatterFrand(rng);
    };

    const float kReach = 1150.0f;
    const float kCell = 13.0f;
    const float kHeightSample = 4.0f;
    const int kMaxInstances = 28000;
    const float kGroveBase = 0.22f;
    const float kGroveGain = 0.62f;
    const float kScaleMin = 0.8f;
    const float kScaleSpan = 1.4f;
    const float kSlopeMax = 5.5f;
    const float reach = kReach, cell = kCell, hs = kHeightSample;
    int placed = 0;
    out.reserve(static_cast<std::size_t>(kMaxInstances) / 4);
    for (float dz = -reach; dz <= reach && placed < kMaxInstances; dz += cell) {
        for (float dx = -reach; dx <= reach && placed < kMaxInstances; dx += cell) {
            const float grove = frand();
            if (frand() > (kGroveBase + kGroveGain * grove))
                continue;
            const float x = params.anchor_x + dx + (frand() * 2.0f - 1.0f) * cell * 0.5f;
            const float zc = params.anchor_z + dz + (frand() * 2.0f - 1.0f) * cell * 0.5f;
            const float h = cbs.terrain_height(x, zc);

            if (h < cbs.water_level(x, zc) + 1.0f)
                continue;

            {
                bool roofed = false;
                for (float up = 1.0f; up <= 6.0f; up += 1.0f) {
                    if (cbs.density_at(x, h + up, zc) < 0.0f) {
                        roofed = true;
                        break;
                    }
                }
                if (roofed)
                    continue;
            }
            const float slope = glm::max(glm::max(std::abs(cbs.terrain_height(x + hs, zc) - h),
                                                  std::abs(cbs.terrain_height(x - hs, zc) - h)),
                                         glm::max(std::abs(cbs.terrain_height(x, zc + hs) - h),
                                                  std::abs(cbs.terrain_height(x, zc - hs) - h)));
            if (slope > kSlopeMax)
                continue;

            const float s = kScaleMin + frand() * kScaleSpan;
            const glm::vec3 treePos(x, h, zc);
            const auto treeRot = glm::angleAxis(frand() * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));

            auto pgen = luminumbra::core::DeterministicRng::seeded(
                luminumbra::foliage::kPlantSeedOffset,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.x)) * 0x9E3779B1ull,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.z)) * 0x85EBCA77ull);
            const auto pgenome = luminumbra::foliage::RandomGenome(pgen);
            const float maturity = pgen.next_unit();

            const float maturityScale = 0.5f + 0.5f * maturity;
            const float geneticSize =
                0.7f + luminumbra::foliage::ExpressGenome(pgenome).max_scale * 0.21f;
            const float effScale = s * maturityScale * geneticSize;

            TreePlacement t;
            t.position = treePos;
            t.rotation = treeRot;
            t.eff_scale = effScale;
            if (params.tree_palette_count > 0) {
                t.palette_index = static_cast<int>(
                    (static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.x) * 73856093) ^
                     static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.z) * 19349663)) %
                    static_cast<std::uint64_t>(params.tree_palette_count));
            }
            out.push_back(t);
            ++placed;
        }
    }
    return out;
}

std::vector<RockPlacement> ComputeRockPlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng) {
    std::vector<RockPlacement> out;

    if (params.rock_palette_count <= 0)
        return out;
    auto frand = [&rng]() {
        return ScatterFrand(rng);
    };
    const float rReach = 900.0f;
    const float rCell = 19.0f;
    const float rHS = 3.0f;
    const int rCap = 14000;
    int rocksPlaced = 0;
    for (float gz = -rReach; gz <= rReach && rocksPlaced < rCap; gz += rCell) {
        for (float gx = -rReach; gx <= rReach && rocksPlaced < rCap; gx += rCell) {
            const float rx = params.anchor_x + gx + (frand() - 0.5f) * rCell;
            const float rz = params.anchor_z + gz + (frand() - 0.5f) * rCell;
            const float hC = cbs.terrain_height(rx, rz);
            if (hC <= cbs.water_level(rx, rz) + 0.3f)
                continue;
            const float sx = cbs.terrain_height(rx + rHS, rz) - cbs.terrain_height(rx - rHS, rz);
            const float sz = cbs.terrain_height(rx, rz + rHS) - cbs.terrain_height(rx, rz - rHS);
            const float slope = std::sqrt(sx * sx + sz * sz) / rHS;

            const float density = 0.05f + std::min(slope * 0.9f, 0.7f);
            if (frand() >= density)
                continue;
            const int pidx = static_cast<int>(
                (static_cast<std::uint64_t>(static_cast<std::int64_t>(rx) * 73856093) ^
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(rz) * 19349663)) %
                static_cast<std::uint64_t>(params.rock_palette_count));
            float s = 0.8f + frand() * 2.2f;
            if (frand() > 0.93f)
                s *= 2.6f;
            RockPlacement r;
            r.position = glm::vec3(rx, hC - 0.35f * s, rz);
            r.scale = glm::vec3(s, s * (0.7f + 0.5f * frand()), s);
            r.rotation = glm::angleAxis(frand() * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
            r.palette_index = pidx;
            out.push_back(r);
            ++rocksPlaced;
        }
    }
    return out;
}

std::vector<BushPlacement> ComputeBushPlacements(const WorldDressingParams& params,
                                                 const WorldDressingCallbacks& cbs,
                                                 std::uint64_t& rng) {
    std::vector<BushPlacement> out;

    if (params.bush_palette_count <= 0)
        return out;
    auto frand = [&rng]() {
        return ScatterFrand(rng);
    };
    const float bReach = 850.0f;
    const float bCell = 8.0f;
    const float bHS = 3.0f;
    const int bCap = 40000;
    int bushPlaced = 0;
    for (float gz = -bReach; gz <= bReach && bushPlaced < bCap; gz += bCell) {
        for (float gx = -bReach; gx <= bReach && bushPlaced < bCap; gx += bCell) {
            const float bx = params.anchor_x + gx + (frand() - 0.5f) * bCell;
            const float bz = params.anchor_z + gz + (frand() - 0.5f) * bCell;
            const float hC = cbs.terrain_height(bx, bz);
            if (hC <= cbs.water_level(bx, bz) + 0.5f)
                continue;
            const float sx = cbs.terrain_height(bx + bHS, bz) - cbs.terrain_height(bx - bHS, bz);
            const float sz = cbs.terrain_height(bx, bz + bHS) - cbs.terrain_height(bx, bz - bHS);
            const float slope = std::sqrt(sx * sx + sz * sz) / bHS;
            if (slope > 0.85f)
                continue;

            const float veg = cbs.vegetation_density(bx, bz);

            const float density =
                std::min(1.0f, veg * 1.8f) * (1.0f - std::min(slope, 0.7f) * 0.85f);
            if (frand() >= density)
                continue;
            const int pidx = static_cast<int>(
                (static_cast<std::uint64_t>(static_cast<std::int64_t>(bx) * 73856093) ^
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(bz) * 19349663)) %
                static_cast<std::uint64_t>(params.bush_palette_count));
            float s = 1.1f + frand() * 1.6f;
            if (frand() > 0.95f)
                s *= 1.9f;
            BushPlacement b;
            b.position = glm::vec3(bx, hC - 0.12f * s, bz);
            b.scale = glm::vec3(s, s * (0.7f + 0.4f * frand()), s);
            b.rotation = glm::angleAxis(frand() * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
            b.palette_index = pidx;
            out.push_back(b);
            ++bushPlaced;
        }
    }
    return out;
}

std::vector<CreaturePlacement> ComputeWildlifePlacements(const WorldDressingParams& params,
                                                         const WorldDressingCallbacks& cbs) {
    std::vector<CreaturePlacement> out;

    auto wgen = luminumbra::core::DeterministicRng::seeded(0xFA0FA0u, 4242u, 1u);
    int wlHoles = 0;
    for (int i = 0; i < params.herd_count; ++i) {
        const float ang = wgen.next_unit() * 6.2831853f;
        const float rad = 10.0f + wgen.next_unit() * 60.0f;
        const float wx = params.anchor_x + std::cos(ang) * rad;
        const float wz = params.anchor_z + std::sin(ang) * rad;
        const float gy = cbs.terrain_height(wx, wz);
        if (gy <= cbs.water_level(wx, wz) + 0.3f) {
            if (wlHoles < 3) {
                CreaturePlacement hole;
                hole.kind = CreaturePlacement::Kind::WaterHole;
                hole.position = glm::vec3(wx, cbs.water_level(wx, wz), wz);
                out.push_back(hole);
                ++wlHoles;
            }
            continue;
        }

        const int species_index =
            cbs.species_for_biome(cbs.biome_name(wx, wz), static_cast<std::size_t>(i));
        const float size = 0.8f + wgen.next_unit() * 0.7f;

        luminumbra::creature::CreatureBuildGenome bg;
        bg.height = wgen.next_unit();
        bg.girth = wgen.next_unit();
        bg.length = wgen.next_unit();
        bg.size = size;
        const luminumbra::creature::CreatureBuild build =
            luminumbra::creature::ComputeCreatureBuild(bg);
        CreaturePlacement c;
        c.kind = CreaturePlacement::Kind::Creature;
        c.position = glm::vec3(wx, gy + 1.2f, wz);
        c.yaw = ang;
        c.species_index = species_index;
        c.size = size;
        c.build_scale = glm::vec3(build.scale_x, build.scale_y, build.scale_z);
        c.female = (i % 2 == 0);

        c.thirst = 0.1f + wgen.next_unit() * 0.25f;
        c.anim_phase = wgen.next_unit();
        out.push_back(c);
    }
    return out;
}

WorldDressingResult ComputeWorldDressing(const WorldDressingParams& params,
                                         const WorldDressingCallbacks& cbs) {
    WorldDressingResult result;

    std::uint64_t rng = InitialScatterRngState(params.anchor_x, params.anchor_z);
    result.trees = LegacyDressingGolden::ComputeTreePlacements(params, cbs, rng);
    result.rocks = LegacyDressingGolden::ComputeRockPlacements(params, cbs, rng);
    result.bushes = LegacyDressingGolden::ComputeBushPlacements(params, cbs, rng);
    if (params.compute_wildlife) {
        result.wildlife = LegacyDressingGolden::ComputeWildlifePlacements(params, cbs);
    }
    return result;
}

// Frozen legacy consume logic; independent of the production helpers.
void ConsumeDressingPlacement(entt::registry& reg, const TreePlacement& t) {
    if (t.palette_index < 0)
        return; // empty palette: counted, nothing to emit
    const Luminumbra::Vec3 treeScale(t.eff_scale, t.eff_scale, t.eff_scale);
    auto emit = [&](const std::string& meshKey, std::uint32_t mat) {
        const auto e = reg.create();
        auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
        tf.position = t.position;
        tf.scale = treeScale;
        tf.rotation = t.rotation;
        auto& sm = reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
        sm.meshPath = meshKey;
        sm.materialId = mat;
    };
    emit(std::string("game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_") +
             "trunk.lmesh",
         2u);
    emit(std::string("game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_") +
             "branches.lmesh",
         2u);
    emit(std::string("game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_") +
             "leaves.lmesh",
         3u);
}

void ConsumeDressingPlacement(entt::registry& reg, const RockPlacement& r) {
    const auto e = reg.create();
    auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
    tf.position = r.position; // settled into ground
    tf.scale = r.scale;
    tf.rotation = r.rotation;
    auto& sm = reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
    sm.meshPath = "procgen://rock_" + std::to_string(r.palette_index);
    sm.materialId = 1u; // Stone -> stone triplanar texture
}

void ConsumeDressingPlacement(entt::registry& reg, const BushPlacement& b) {
    const auto e = reg.create();
    auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
    tf.position = b.position; // settled into ground
    tf.scale = b.scale;
    tf.rotation = b.rotation;
    auto& sm = reg.emplace<Luminumbra::Components::StaticMeshComponent>(e);
    sm.meshPath = "procgen://bush_" + std::to_string(b.palette_index);
    sm.materialId = 3u; // grass/green leaf material (shrub foliage)
}
bool ConsumeDressingWildlife(entt::registry& reg,
                             const CreaturePlacement& c,
                             const std::vector<luminumbra::ai::CreatureSpecies>& roster,
                             const DressingWildlifeOptions& options) {
    namespace anim = luminumbra::animation;
    if (c.kind == Luminumbra::Client::CreaturePlacement::Kind::WaterHole) {
        // A DRINKING SPOT at the water so the wired thirst
        // system has somewhere to steer creatures.
        const auto he = reg.create();
        auto& htf = reg.emplace<Luminumbra::Components::TransformComponent>(he);
        htf.position = c.position;
        reg.emplace<Luminumbra::Components::WaterHoleComponent>(he).radius = 6.0f;
        return false;
    }
    if (c.species_index < 0 || static_cast<std::size_t>(c.species_index) >= roster.size())
        return false; // defensive: the roster never changes mid-session
    const auto& sp = roster[static_cast<std::size_t>(c.species_index)];
    const auto e = reg.create();
    auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
    tf.position = c.position; // settle onto ground (gy + 1.2)
    tf.scale = Luminumbra::Vec3(c.build_scale.x, c.build_scale.y, c.build_scale.z);
    tf.rotation = glm::angleAxis(c.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    auto& sm = reg.emplace<Luminumbra::Components::SkinnedMeshComponent>(e);
    sm.meshPath = "data/models/creatures/grovestrider/grovestrider.lmesh";
    sm.materialId = 3u;
    sm.tintR = sp.base_color[0];
    sm.tintG = sp.base_color[1];
    sm.tintB = sp.base_color[2];
    auto& cr = reg.emplace<Luminumbra::Components::CreatureComponent>(e);
    cr.species_id = sp.species_id();
    cr.is_predator = sp.predator;
    cr.hunger = options.init_hunger;
    cr.move_speed = sp.predator ? options.pred_speed : options.prey_speed;
    auto& gn = reg.emplace<Luminumbra::Components::CreatureGenomeComponent>(e);
    gn.move_speed = cr.move_speed;
    gn.size_scale = c.size;
    gn.female = c.female;
    gn.age_ticks = 100u;
    // Survival: every creature thirsts (seeks the drinking spots
    // above); predators also scavenge carrion. Activates the wired
    // Thirst/Scavenging tick systems in the living world.
    auto& th = reg.emplace<Luminumbra::Components::ThirstComponent>(e);
    th.thirst = c.thirst;
    if (sp.predator)
        reg.emplace<Luminumbra::Components::ScavengerComponent>(e);
    // a circadian clock -> the creature sleeps in its
    // off-phase (diurnal at night, nocturnal by day). The brain's
    // Sleep utility reads CircadianComponent.activity. Nocturnal is
    // now a per-species DATA flag (creatures/species/*.json), so a new
    // species can be nocturnal without a client recompile.
    reg.emplace<Luminumbra::Components::CircadianComponent>(e).nocturnal = sp.nocturnal ? 1u : 0u;
    // the built-but-unstamped behavior
    // components join the CLIENT ambient roster (never feeds server
    // hashes — this registry is client decoration): prey herds get
    // ALARM vigilance (collective flee), predators get PACK-HUNTER
    // flank coordination; everyone claims a TERRITORY home range at
    // its spawn point and carries the MIGRATORY seasonal-drive slot.
    // Mortal/Decay lifecycle prep: a long seeded lifespan so ambient
    // populations turn over slowly (the calm-demo's fast lifecycle
    // stays demo-only).
    if (sp.predator) {
        reg.emplace<Luminumbra::Components::PackHunterComponent>(e);
    } else {
        reg.emplace<Luminumbra::Components::AlarmComponent>(e);
    }
    {
        auto& terr_home = reg.emplace<Luminumbra::Components::TerritoryComponent>(e);
        terr_home.home_x = c.position.x;
        terr_home.home_z = c.position.z;
        terr_home.radius = sp.predator ? 60.0f : 35.0f;
        terr_home.established = 1;
        reg.emplace<Luminumbra::Components::MigratoryComponent>(e);
        auto& mort = reg.emplace<Luminumbra::Components::MortalComponent>(e);
        const std::uint32_t jit =
            static_cast<std::uint32_t>(entt::to_integral(e) * 2654435761u) % 36000u;
        mort.lifespan_ticks = 108000u + jit; // ~60-80 min @30Hz ambient turnover
        reg.emplace<Luminumbra::Components::DecayComponent>(e).decay_duration = 900u;
    }
    auto& pl = reg.emplace<anim::AnimationPlayerComponent>(e);
    pl.skeleton = options.skeleton;
    pl.clip = options.clip;
    pl.time = c.anim_phase * 2.0; // staggered phase
    pl.looping = true;
    if (options.physics) {
        const std::size_t idx = options.physics->create_avatar_character(
            glm::vec3(c.position.x, c.position.y, c.position.z));
        reg.emplace<Luminumbra::Components::CreaturePhysicsComponent>(e, idx);
    }
    return true;
}

} // namespace LegacyDressingGolden

namespace {
// Capture actual registry/animation/controller outputs, field by field (no padding).
std::string ConsumedBytes(entt::registry& registry,
                          const luminumbra::animation::Skeleton& skeleton,
                          const luminumbra::animation::AnimationClip& clip,
                          Luminumbra::Systems::PhysicsSystem& physics) {
    namespace C = Luminumbra::Components;
    FnvHasher out;
    std::vector<entt::entity> entities;
    for (const auto entity : registry.view<C::TransformComponent>())
        entities.push_back(entity);
    std::sort(entities.begin(), entities.end(), [](auto a, auto b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    for (const auto entity : entities) {
        out.field(entt::to_integral(entity));
        const auto& tf = registry.get<C::TransformComponent>(entity);
        out.vec3(tf.position);
        out.vec3(tf.scale);
        out.quat(tf.rotation);
        const auto* mesh = registry.try_get<C::StaticMeshComponent>(entity);
        out.field(mesh != nullptr);
        if (mesh) {
            out.field(mesh->meshPath.size());
            out.emitted += mesh->meshPath;
            out.field(mesh->materialId);
        }
        const auto* skin = registry.try_get<C::SkinnedMeshComponent>(entity);
        out.field(skin != nullptr);
        if (skin) {
            out.field(skin->meshPath.size());
            out.emitted += skin->meshPath;
            out.field(skin->materialId);
            out.field(skin->tintR);
            out.field(skin->tintG);
            out.field(skin->tintB);
        }
        const auto* water = registry.try_get<C::WaterHoleComponent>(entity);
        out.field(water != nullptr);
        if (water)
            out.field(water->radius);
        const auto* player =
            registry.try_get<luminumbra::animation::AnimationPlayerComponent>(entity);
        out.field(player != nullptr);
        if (player) {
            EXPECT_EQ(player->skeleton, &skeleton);
            EXPECT_EQ(player->clip, &clip);
            out.field(player->time);
            out.field(player->looping);
            out.field(player->pose.joints.size());
            out.field(player->palette.size());
        }
        const auto* body = registry.try_get<C::CreaturePhysicsComponent>(entity);
        out.field(body != nullptr);
        if (body) {
            out.field(body->avatar_index);
            out.vec3(physics.get_avatar_position(body->avatar_index));
            out.vec3(physics.get_avatar_velocity(body->avatar_index));
            out.field(physics.is_avatar_grounded(body->avatar_index));
        }
        if (registry.all_of<C::CreatureComponent>(entity)) {
            // Add only the metadata required to project the complete legacy
            // simulation components; consumer outputs above remain untouched.
            const auto owner = Luminumbra::world::ActiveRegionLedger::region_at(tf.position);
            registry.emplace<C::CreatureRegionComponent>(
                entity,
                static_cast<std::uint64_t>(entt::to_integral(entity)) + 1u,
                owner.x,
                owner.z);
            registry.emplace<C::CreatureCoarseComponent>(entity);
            registry.emplace<C::CreatureLocomotionStateComponent>(entity);
        }
    }
    out.emitted +=
        luminumbra::ai::SerializeCreatureSnapshot(luminumbra::ai::BuildCreatureSnapshot(registry));
    return out.emitted;
}
} // namespace

TEST(WorldDressing, FlagOffPlacementAndConsumeBytesMatchLegacyGolden) {
    const auto params = SyntheticParams();
    for (const float base : {10.0f, -20.0f}) {
        const auto callbacks = SyntheticCallbacks(base);
        const auto golden = LegacyDressingGolden::ComputeWorldDressing(params, callbacks);
        const auto actual = ComputeWorldDressing(params, callbacks);
        EXPECT_EQ(HashPlacements(actual), HashPlacements(golden));
        EXPECT_EQ(PlacementFields(actual).emitted, PlacementFields(golden).emitted);
        entt::registry consumed, legacy;
        luminumbra::animation::Skeleton skeleton;
        luminumbra::animation::AnimationClip clip;
        Luminumbra::Systems::PhysicsSystem physics, legacy_physics;
        physics.startup();
        legacy_physics.startup();
        std::vector<luminumbra::ai::CreatureSpecies> roster(5);
        for (std::size_t i = 0; i < roster.size(); ++i) {
            roster[i].id = "consumer_species_" + std::to_string(i);
            roster[i].predator = i % 2 == 0;
            roster[i].nocturnal = i % 3 == 0;
            roster[i].base_color[0] = static_cast<float>(i) * 0.125f;
        }
        for (const auto& t : actual.trees)
            Luminumbra::Client::ConsumeDressingPlacement(consumed, t);
        for (const auto& r : actual.rocks)
            Luminumbra::Client::ConsumeDressingPlacement(consumed, r);
        for (const auto& b : actual.bushes)
            Luminumbra::Client::ConsumeDressingPlacement(consumed, b);
        for (const auto& t : golden.trees)
            LegacyDressingGolden::ConsumeDressingPlacement(legacy, t);
        for (const auto& r : golden.rocks)
            LegacyDressingGolden::ConsumeDressingPlacement(legacy, r);
        for (const auto& b : golden.bushes)
            LegacyDressingGolden::ConsumeDressingPlacement(legacy, b);
        std::size_t spawned = 0, legacy_spawned = 0;
        const Luminumbra::Client::DressingWildlifeOptions options{
            false, 0.3125f, 4.0f, 2.0f, &skeleton, &clip, &physics};
        auto legacy_options = options;
        legacy_options.physics = &legacy_physics;
        for (const auto& c : actual.wildlife)
            spawned += Luminumbra::Client::ConsumeDressingWildlife(consumed, c, roster, options);
        for (const auto& c : golden.wildlife)
            legacy_spawned +=
                LegacyDressingGolden::ConsumeDressingWildlife(legacy, c, roster, legacy_options);
        EXPECT_EQ(spawned, legacy_spawned);
        EXPECT_EQ(ConsumedBytes(consumed, skeleton, clip, physics),
                  ConsumedBytes(legacy, skeleton, clip, legacy_physics));
        if (base > 0.0f) {
            EXPECT_GT(spawned, 0u);
            EXPECT_EQ(consumed.view<Luminumbra::Components::CreaturePhysicsComponent>().size(),
                      spawned);
        } else {
            EXPECT_EQ(spawned, 0u);
            EXPECT_GT(consumed.view<Luminumbra::Components::WaterHoleComponent>().size(), 0u);
        }
        EXPECT_EQ(actual.wildlife.size(), golden.wildlife.size());
        EXPECT_FALSE(actual.wildlife.empty());
    }
}

namespace {
entt::entity PresentationCreature(entt::registry& registry, std::uint64_t id) {
    namespace C = Luminumbra::Components;
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::CreatureComponent>(entity).species_id = std::uint16_t{1};
    registry.emplace<C::CreatureRegionComponent>(entity).persistent_id = id;
    registry.emplace<C::CreatureCoarseComponent>(entity);
    registry.emplace<C::CreatureControllerComponent>(entity);
    return entity;
}
} // namespace

TEST(WorldDressing, EnabledDressingAttachesPersistedCreaturesWithoutSpawning) {
    namespace A = luminumbra::ai;
    entt::registry registry;
    const auto second = PresentationCreature(registry, 2);
    const auto first = PresentationCreature(registry, 1);
    const auto before = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(registry));
    auto params = SyntheticParams();
    params.active_regions = true;
    EXPECT_TRUE(ComputeWorldDressing(params, SyntheticCallbacks()).wildlife.empty());
    const auto attachments = Luminumbra::Client::CreaturePresentationAttachments(registry);
    ASSERT_EQ(attachments.size(), 2u);
    EXPECT_EQ(attachments[0].persistent_id, 1u);
    EXPECT_EQ(attachments[0].entity, first);
    EXPECT_EQ(attachments[1].entity, second);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(registry)), before);
}

TEST(WorldDressing, EnabledRestartDoesNotDuplicateAmbientCreaturesOrWaterHoles) {
    namespace A = luminumbra::ai;
    namespace C = Luminumbra::Components;
    entt::registry original, restored;
    PresentationCreature(original, 7);
    const auto snapshot = A::BuildCreatureSnapshot(original);
    A::ApplyCreatureSnapshot(restored, snapshot);
    const auto before = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored));
    auto params = SyntheticParams();
    params.active_regions = true;
    for (int frame = 0; frame < 3; ++frame) {
        EXPECT_TRUE(ComputeWorldDressing(params, SyntheticCallbacks(-20.0f)).wildlife.empty());
        const auto attachments = Luminumbra::Client::CreaturePresentationAttachments(restored);
        ASSERT_EQ(attachments.size(), 1u);
        EXPECT_EQ(attachments[0].persistent_id, 7u);
    }
    EXPECT_EQ(restored.view<C::CreatureComponent>().size(), 1u);
    EXPECT_TRUE(restored.view<C::WaterHoleComponent>().empty());
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)), before);
}
