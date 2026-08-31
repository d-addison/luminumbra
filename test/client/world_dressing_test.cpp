// CPU-only determinism coverage for the extracted
// world-dressing placement computation (src/luminumbra_client/WorldDressing.cpp).
// The computation was moved VERBATIM out of main_client's first-IN_GAME-frame
// bring-up so it can run on a background job; the load-bearing property is that
// it is a pure, order-stable function of (params, callbacks) — the same inputs
// must yield a byte-identical placement stream every run, and the tree/rock/bush
// loops must share ONE sequential RNG stream (rocks continue after trees, bushes
// after rocks). A pinned literal hash would break on any legitimate future
// tuning, so the honest scope here is self-consistency + determinism over
// synthetic callbacks (no GL, no world system, no entt).

#include "WorldDressing.h"

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
    void bytes(const void* p, std::size_t n) {
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

std::uint64_t HashPlacements(const WorldDressingResult& r) {
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
    return f.h;
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
