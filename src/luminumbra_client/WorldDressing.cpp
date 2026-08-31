// the world-dressing placement loops, moved VERBATIM from
// main_client.cpp's first-IN_GAME-frame bring-up (see WorldDressing.h for the
// contract). Every constant, RNG draw, and rejection test below is the legacy
// inline code with only the world queries swapped for the injected callbacks —
// do not "clean up" the arithmetic or reorder draws: the seeded layout must stay
// byte-identical to what shipped inline.

#include "WorldDressing.h"

#include "luminumbra_common/core/DeterministicRng.h"
#include "luminumbra_common/systems/CreatureProcgen.h"   // genome -> body-proportion build
#include "luminumbra_common/systems/PlantGrowthSystem.h" //  phenotype/genome

#include <algorithm>
#include <cmath>

namespace Luminumbra::Client {

namespace {

// The legacy per-candidate frand: a SplitMix64 step over the SHARED scatter
// stream (moved verbatim — including the double-precision mantissa division).
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
    //  trees: the seed derivation from the spawn anchor (moved verbatim).
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
    //  -grove tree-scatter knobs (render-only client decoration, never
    // hashed). Lower base + higher grove gain reads as clustered copses (sparse
    // open ground, dense stands) instead of a uniform sprinkle. Per-cell frand
    // call order is unchanged so the seeded layout stays reproducible.
    // VAST FOREST: the trees are a small PALETTE of procedural meshes INSTANCED
    // thousands of times across the whole horizon (cheap GPU instancing +
    // tree rendering LOD + frustum cull), so the full reach fills densely without
    // per-tree cost.
    const float kReach = 1150.0f;     // meters from spawn anchor (deep horizon)
    const float kCell = 13.0f;        // grid pitch
    const float kHeightSample = 4.0f; // slope probe radius
    const int kMaxInstances = 28000;  // instance cap (cheap palette instances; far = billboards)
    const float kGroveBase = 0.22f;   // baseline grove density (sparser open)
    const float kGroveGain = 0.62f;   // grove clustering gain (denser stands)
    const float kScaleMin = 0.8f;     // min trunk scale
    const float kScaleSpan = 1.4f;    // scale jitter span -> 0.8..2.2
    const float kSlopeMax = 5.5f;     // skip steeper than this
    const float reach = kReach, cell = kCell, hs = kHeightSample;
    int placed = 0;
    out.reserve(static_cast<std::size_t>(kMaxInstances) / 4); // typical fill is well under cap
    for (float dz = -reach; dz <= reach && placed < kMaxInstances; dz += cell) {
        for (float dx = -reach; dx <= reach && placed < kMaxInstances; dx += cell) {
            // Clustered density: a low-frequency mask makes groves
            // (denser stands) instead of a uniform sprinkle.
            const float grove = frand();
            if (frand() > (kGroveBase + kGroveGain * grove))
                continue;
            const float x = params.anchor_x + dx + (frand() * 2.0f - 1.0f) * cell * 0.5f;
            const float zc = params.anchor_z + dz + (frand() * 2.0f - 1.0f) * cell * 0.5f;
            const float h = cbs.terrain_height(x, zc);
            // Keep trees out of water at ANY elevation (sea + perched
            // lakes), not just sea level; a shoreline margin is fine.
            if (h < cbs.water_level(x, zc) + 1.0f)
                continue;
            // ROOFED-CAVE REJECT (cave bug A): terrain_height is the analytic
            // heightmap surface and ignores the cave SDF, so a column whose
            // heightmap point sits under a cavern roof would grow a tree deep
            // underground. Reject if SOLID terrain lies just above the surface.
            // DENSITY CONVENTION ( root cause — this probe shipped
            // INVERTED and rejected every open column, removing all trees):
            // SOLID = density < 0; air = >= 0.
            // Render-only scatter (never hashed) -> pure SDF read, no re-pin.
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
                continue; // skip steep/cliff
            // frand order (scale then rotation) is unchanged so the
            // seeded layout stays reproducible.
            const float s = kScaleMin + frand() * kScaleSpan;
            const glm::vec3 treePos(x, h, zc);
            const auto treeRot = glm::angleAxis(frand() * 6.2831853f, glm::vec3(0.0f, 1.0f, 0.0f));
            //  bake deterministic GENETIC + MATURITY size variation so
            // the grove reads as GROWN — a mix of saplings.. mature trees from a
            // position-seeded plant genome + age. Uses its OWN rng stream so the
            // frand layout above is unchanged.
            auto pgen = luminumbra::core::DeterministicRng::seeded(
                luminumbra::foliage::kPlantSeedOffset,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.x)) * 0x9E3779B1ull,
                static_cast<std::uint64_t>(static_cast<std::int64_t>(treePos.z)) * 0x85EBCA77ull);
            const auto pgenome = luminumbra::foliage::RandomGenome(pgen);
            const float maturity = pgen.next_unit(); // 0 sapling.. 1 mature
            // Bias toward mature trees so the procedural wood reads full
            // (fewer tiny saplings than the old baked-grove distribution).
            const float maturityScale = 0.5f + 0.5f * maturity;
            const float geneticSize =
                0.7f + luminumbra::foliage::ExpressGenome(pgenome).max_scale * 0.21f; // ~0.83..1.2
            const float effScale = s * maturityScale * geneticSize;
            // PROGRAMMATIC TREES, NO MODEL: pick a palette entry (by position
            // hash). The consume side spawns TWO instanced static-mesh entities
            // (bark + leaf) at this transform. -1 = empty palette: the candidate
            // still counts toward the cap/log, nothing is emitted (legacy gate).
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
    // ROCK FORMATIONS (worldgen-richness ): DENSER on steep terrain
    // (scree / rocky outcrops), a sparse baseline of boulders on open ground,
    // and rare large sentinels.  (never hashed); the frand stream
    // simply continues after the trees, so the layout stays deterministic +
    // reproducible. An empty palette skips the WHOLE loop (and its draws),
    // exactly as the legacy `if (g_rockPaletteCount > 0)` gate did.
    if (params.rock_palette_count <= 0)
        return out;
    auto frand = [&rng]() {
        return ScatterFrand(rng);
    };
    const float rReach = 900.0f; // metres from spawn anchor
    const float rCell = 19.0f;   // grid pitch
    const float rHS = 3.0f;      // slope probe radius
    const int rCap = 14000;      // instance cap
    int rocksPlaced = 0;
    for (float gz = -rReach; gz <= rReach && rocksPlaced < rCap; gz += rCell) {
        for (float gx = -rReach; gx <= rReach && rocksPlaced < rCap; gx += rCell) {
            const float rx = params.anchor_x + gx + (frand() - 0.5f) * rCell;
            const float rz = params.anchor_z + gz + (frand() - 0.5f) * rCell;
            const float hC = cbs.terrain_height(rx, rz);
            if (hC <= cbs.water_level(rx, rz) + 0.3f)
                continue; // not underwater (sea or lake)
            const float sx = cbs.terrain_height(rx + rHS, rz) - cbs.terrain_height(rx - rHS, rz);
            const float sz = cbs.terrain_height(rx, rz + rHS) - cbs.terrain_height(rx, rz - rHS);
            const float slope = std::sqrt(sx * sx + sz * sz) / rHS;
            // baseline boulders on flats; many more on slopes (scree).
            const float density = 0.05f + std::min(slope * 0.9f, 0.7f);
            if (frand() >= density)
                continue;
            const int pidx = static_cast<int>(
                (static_cast<std::uint64_t>(static_cast<std::int64_t>(rx) * 73856093) ^
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(rz) * 19349663)) %
                static_cast<std::uint64_t>(params.rock_palette_count));
            float s = 0.8f + frand() * 2.2f; // small..medium
            if (frand() > 0.93f)
                s *= 2.6f; // rare sentinel boulders
            RockPlacement r;
            r.position = glm::vec3(rx, hC - 0.35f * s, rz); // settle into ground
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
    // SHRUB/BUSH LAYER: scatter on flatter, vegetated ground —
    // driven by the per-biome vegetation density (the same signal that drives
    // the grass scatter), so deserts/rock stay sparse and forests/meadows fill
    // with shrubs. The niche is the COMPLEMENT of the rocks: bushes on
    // flats/gentle slopes, rocks on scree.  (never hashed); the
    // frand stream continues after the rocks so the layout stays
    // deterministic. Empty palette skips the whole loop (legacy gate).
    if (params.bush_palette_count <= 0)
        return out;
    auto frand = [&rng]() {
        return ScatterFrand(rng);
    };
    const float bReach = 850.0f; // metres from spawn anchor
    const float bCell = 8.0f;    // grid pitch (lush undergrowth, denser than trees)
    const float bHS = 3.0f;      // slope probe radius
    const int bCap = 40000;      // instance cap
    int bushPlaced = 0;
    for (float gz = -bReach; gz <= bReach && bushPlaced < bCap; gz += bCell) {
        for (float gx = -bReach; gx <= bReach && bushPlaced < bCap; gx += bCell) {
            const float bx = params.anchor_x + gx + (frand() - 0.5f) * bCell;
            const float bz = params.anchor_z + gz + (frand() - 0.5f) * bCell;
            const float hC = cbs.terrain_height(bx, bz);
            if (hC <= cbs.water_level(bx, bz) + 0.5f)
                continue; // not in water
            const float sx = cbs.terrain_height(bx + bHS, bz) - cbs.terrain_height(bx - bHS, bz);
            const float sz = cbs.terrain_height(bx, bz + bHS) - cbs.terrain_height(bx, bz - bHS);
            const float slope = std::sqrt(sx * sx + sz * sz) / bHS;
            if (slope > 0.85f)
                continue; // shrubs avoid steep/cliff (rocks own that)
            // per-biome vegetation density gates cover; flats favoured.
            const float veg = cbs.vegetation_density(bx, bz);
            // Undergrowth is lush on vegetated flats; falls off on slope.
            const float density =
                std::min(1.0f, veg * 1.8f) * (1.0f - std::min(slope, 0.7f) * 0.85f);
            if (frand() >= density)
                continue;
            const int pidx = static_cast<int>(
                (static_cast<std::uint64_t>(static_cast<std::int64_t>(bx) * 73856093) ^
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(bz) * 19349663)) %
                static_cast<std::uint64_t>(params.bush_palette_count));
            float s = 1.1f + frand() * 1.6f; // small..medium shrubs
            if (frand() > 0.95f)
                s *= 1.9f; // rare large bush
            BushPlacement b;
            b.position = glm::vec3(bx, hC - 0.12f * s, bz); // settle into ground
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
    // LIVING WORLD ambient-wildlife candidate loop (moved verbatim). Its own
    // DeterministicRng stream — independent of the scatter frand stream. A
    // water-cell candidate `continue`s after only the ang/rad draws, exactly as
    // the legacy loop did, so the stream position of every later creature is
    // preserved. The EnTT emplaces / physics avatars / skinned-rig pointers are
    // NOT computed here — the consume side owns those (main-thread-only).
    auto wgen = luminumbra::core::DeterministicRng::seeded(0xFA0FA0u, 4242u, 1u);
    int wlHoles = 0;
    for (int i = 0; i < params.herd_count; ++i) {
        const float ang = wgen.next_unit() * 6.2831853f;
        const float rad = 10.0f + wgen.next_unit() * 60.0f;
        const float wx = params.anchor_x + std::cos(ang) * rad;
        const float wz = params.anchor_z + std::sin(ang) * rad;
        const float gy = cbs.terrain_height(wx, wz);
        if (gy <= cbs.water_level(wx, wz) + 0.3f) {
            // Water cell: drop a few DRINKING SPOTS at the water so the
            // wired thirst system has somewhere to steer creatures. Capped.
            if (wlHoles < 3) {
                CreaturePlacement hole;
                hole.kind = CreaturePlacement::Kind::WaterHole;
                hole.position = glm::vec3(wx, cbs.water_level(wx, wz), wz);
                out.push_back(hole);
                ++wlHoles;
            }
            continue; // not in water
        }
        // Biome-appropriate species: pick among the species that inhabit the
        // local biome (generalists included); the injected resolver applies the
        // fall-back-to-full-roster rule. No RNG draws in selection (pure pick).
        const int species_index =
            cbs.species_for_biome(cbs.biome_name(wx, wz), static_cast<std::size_t>(i));
        const float size = 0.8f + wgen.next_unit() * 0.7f; // overall size multiplier
        // Procedural BUILD: non-uniform body proportions from sampled build
        // genes give each creature a distinct silhouette (tall/stocky/long)
        // from the same mesh (CreatureProcgen).
        luminumbra::creature::CreatureBuildGenome bg;
        bg.height = wgen.next_unit();
        bg.girth = wgen.next_unit();
        bg.length = wgen.next_unit();
        bg.size = size;
        const luminumbra::creature::CreatureBuild build =
            luminumbra::creature::ComputeCreatureBuild(bg);
        CreaturePlacement c;
        c.kind = CreaturePlacement::Kind::Creature;
        c.position = glm::vec3(wx, gy + 1.2f, wz); // settle onto ground
        c.yaw = ang;
        c.species_index = species_index;
        c.size = size;
        c.build_scale = glm::vec3(build.scale_x, build.scale_y, build.scale_z);
        c.female = (i % 2 == 0);
        // Survival: every creature thirsts (seeks the drinking spots above).
        c.thirst = 0.1f + wgen.next_unit() * 0.25f;
        c.anim_phase = wgen.next_unit(); // staggered clip phase (consume: * 2.0)
        out.push_back(c);
    }
    return out;
}

WorldDressingResult ComputeWorldDressing(const WorldDressingParams& params,
                                         const WorldDressingCallbacks& cbs) {
    WorldDressingResult result;
    // ONE sequential computation in the legacy order: the scatter stream is
    // shared (rocks continue after trees, bushes after rocks); wildlife runs
    // its own seeded stream afterwards.
    std::uint64_t rng = InitialScatterRngState(params.anchor_x, params.anchor_z);
    result.trees = ComputeTreePlacements(params, cbs, rng);
    result.rocks = ComputeRockPlacements(params, cbs, rng);
    result.bushes = ComputeBushPlacements(params, cbs, rng);
    if (params.compute_wildlife) {
        result.wildlife = ComputeWildlifePlacements(params, cbs);
    }
    return result;
}

} // namespace Luminumbra::Client
