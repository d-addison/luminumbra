#pragma once

// rendering (foliage polish): deterministic PROCEDURAL plant-geometry generator.
// PURE function of (genome, growth stage,
// atmosphere) -> branch-skeleton line list. This REPLACES the baked static tree models:
// plants are grown PROGRAMMATICALLY (recursion), GENETICALLY (genome drives structure),
// and ATMOSPHERICALLY (branches bend toward the sun — phototropism — and could respond
// to light/wind), so two genomes grow structurally different plants, a plant elaborates
// as it matures, and a plant leans toward its light.
//
// DETERMINISM: VISUAL-ONLY — never feeds the sim, never
// touches world_hash. But it is a deterministic pure function so a baked cache is
// reproducible and a future CPU/GPU split agrees. Cross-platform bit-stable: uses
// ONLY Luminumbra::DeterministicMath (libm-free Sin/Cos/Sqrt) + IEEE basic ops, with
// hand-rolled normalize/cross (NOT glm::normalize/glm::rotate, which are libm-backed
// and platform-divergent).

#include "../components/PlantComponents.h"
#include "../core/DeterministicMath.h"
#include "PlantGrowthSystem.h" // ExpressGenome / PlantPhenotype

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace luminumbra::foliage {

namespace dm = ::Luminumbra::DeterministicMath;

// --- deterministic vector helpers (IEEE basic ops + dm::Sqrt only) ---
inline glm::vec3 CrossDet(const glm::vec3& a, const glm::vec3& b) {
    return glm::vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline glm::vec3 NormDet(const glm::vec3& v) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 0.0f)
        return glm::vec3(0.0f);
    const float inv = 1.0f / dm::Sqrt(len2); // IEEE-deterministic sqrt
    return glm::vec3(v.x * inv, v.y * inv, v.z * inv);
}

// Rotate a unit direction by (tilt about a perpendicular, azimuth around `dir`).
inline glm::vec3 RotateBranch(const glm::vec3& dir, float tilt, float az) {
    // Stable orthonormal frame (u, v) perpendicular to dir: avoid the degenerate
    // near-vertical case by switching the reference axis.
    const glm::vec3 ref = (dir.y > 0.99f || dir.y < -0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                            : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 u = NormDet(CrossDet(dir, ref));
    const glm::vec3 v = CrossDet(dir, u); // unit (dir,u orthonormal)
    const float ct = dm::Cos(tilt), st = dm::Sin(tilt);
    const float ca = dm::Cos(az), sa = dm::Sin(az);
    const glm::vec3 child = ct * dir + st * (ca * u + sa * v);
    return NormDet(child);
}

// ATMOSPHERIC growth inputs. The renderer supplies these from the live sky/weather
// (real tick: sun direction from the day/night cycle, light from season/exposure).
// Defaults (sun straight up, no phototropism) reproduce the pure genetic form.
struct PlantEnvDir {
    glm::vec3 sun_dir = glm::vec3(0.0f, 1.0f, 0.0f); // UNIT direction TO the sun
    float phototropism = 0.0f;                       // [0,1] strength of sun-seeking
};

struct ProcgenParams {
    float trunk_len = 1.0f;
    int child_count = 2;       // branches per node (2..3)
    float branch_tilt = 0.6f;  // radians off the parent axis
    float length_ratio = 0.7f; // child length / parent length
    int max_depth = 0;         // recursion depth (from growth stage)
    float photo = 0.0f;        // resolved phototropism strength (atmosphere x genome)
};

inline ProcgenParams
DeriveParams(const Comp::PlantGenomeComponent& g, std::uint8_t stage, const PlantEnvDir& env) {
    using G = Comp::PlantGene;
    const PlantPhenotype ph = ExpressGenome(g);
    ProcgenParams p;
    // 2nd SPECIES from the genome: a hardy genome grows a CONIFER (tall, narrow, near-vertical
    // branches); otherwise a BROADLEAF (shorter, wide, spreading). Distinct silhouettes from
    // the same deterministic generator.
    const bool conifer = g.gene(G::Hardiness) > 0.5f;
    if (conifer) {
        p.trunk_len = ph.max_scale * 1.35f; // tall central leader
        p.child_count = 3;
        p.branch_tilt = 0.16f + g.gene(G::GrowthRate) * 0.14f; // 0.16..0.30 (near-vertical)
        p.length_ratio = 0.55f + g.gene(G::MaxScale) * 0.10f;  // 0.55..0.65 (SHORT side branches)
    } else {
        p.trunk_len = ph.max_scale;                                  // 0.6..2.4
        p.child_count = 2 + (g.gene(G::LeafDensity) > 0.5f ? 1 : 0); // 2 or 3
        p.branch_tilt = 0.60f + g.gene(G::GrowthRate) * 0.45f;       // 0.60..1.05 (spreading)
        p.length_ratio = 0.62f + g.gene(G::MaxScale) * 0.18f;        // 0.62..0.80
    }
    // Stage -> recursion depth: Seed(0) = trunk only; deeper as it matures, capped.
    p.max_depth = stage > 5 ? 5 : static_cast<int>(stage);
    // ATMOSPHERIC x GENETIC: leafier genomes seek light harder. clamp01 keeps it [0,1].
    p.photo = clamp01(env.phototropism * (0.3f + 0.5f * ph.leaf_density));
    return p;
}

inline void GrowBranch(const glm::vec3& base,
                       const glm::vec3& dir,
                       float len,
                       int depth,
                       const ProcgenParams& p,
                       const PlantEnvDir& env,
                       std::vector<glm::vec3>& out) {
    const glm::vec3 tip = base + dir * len;
    out.push_back(base);
    out.push_back(tip); // one skeleton segment (GL line list)
    if (depth >= p.max_depth)
        return;
    constexpr float kGoldenAngle = 2.39996323f; // radians (137.5 deg) — phyllotaxis spread
    for (int i = 0; i < p.child_count; ++i) {
        const float az =
            kGoldenAngle * static_cast<float>(i + 1) + static_cast<float>(depth) * 0.7f;
        glm::vec3 cdir = RotateBranch(dir, p.branch_tilt, az);
        // PHOTOTROPISM: bend the child toward the sun (deterministic blend).
        if (p.photo > 0.0f) {
            cdir = NormDet(cdir * (1.0f - p.photo) + env.sun_dir * p.photo);
        }
        GrowBranch(tip, cdir, len * p.length_ratio, depth + 1, p, env, out);
    }
}

// PURE: identical (genome, stage, env) -> identical vertex bytes on any platform.
// Returns a branch-skeleton line list (pairs of endpoints). Visual-only.
inline std::vector<glm::vec3> GeneratePlantMesh(const Comp::PlantGenomeComponent& genome,
                                                std::uint8_t stage,
                                                const PlantEnvDir& env = {}) {
    std::vector<glm::vec3> out;
    const ProcgenParams p = DeriveParams(genome, stage, env);
    GrowBranch(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), p.trunk_len, 0, p, env, out);
    return out;
}

// --- Rich plant structure: tapered branches (pipe-model radii) + sun-facing leaves ---
// What a renderer tessellates into cylinders + leaf quads. Still pure/deterministic.

struct PlantBranch {
    glm::vec3 a;  // base
    glm::vec3 b;  // tip
    float radius; // pipe-model cross-section radius of this segment
    int depth;
};
struct PlantLeaf {
    glm::vec3 pos;    // attach point (a terminal twig tip)
    glm::vec3 normal; // facing direction (toward the sun under phototropism)
};
struct PlantStructure {
    std::vector<PlantBranch> branches;
    std::vector<PlantLeaf> leaves;
};

inline constexpr float kTwigRadius =
    0.018f; // terminal-twig base radius (pipe-model leaf unit) -> thicker trunks

// Post-order recursion: returns this branch's pipe-model radius. Da Vinci / Borchert-Honda
// pipe model — a parent's cross-section area equals the sum of its children's
// (r_parent = sqrt(Sum r_child^2)) -> a naturally thick trunk tapering to thin twigs.
inline float GrowPlant(const glm::vec3& base,
                       const glm::vec3& dir,
                       float len,
                       int depth,
                       const ProcgenParams& p,
                       const PlantEnvDir& env,
                       PlantStructure& s) {
    const glm::vec3 tip = base + dir * len;
    if (depth >= p.max_depth) {
        s.branches.push_back({base, tip, kTwigRadius, depth});
        // A deterministic CLUSTER of leaves per twig -> a fuller canopy. The cards fan
        // WIDELY around the twig (outward) with only a mild sun bias, so the canopy is
        // visible + leafy from ANY camera angle (cards all facing the sun read edge-on /
        // invisible when the sun is overhead). Spread by the golden angle, staggered up the twig.
        const glm::vec3 base_n =
            NormDet(dir + env.sun_dir * (p.photo * 0.5f)); // outward + mild sun bias
        constexpr int kLeavesPerTwig = 5;
        constexpr float kGoldenAngle = 2.39996323f;
        for (int li = 0; li < kLeavesPerTwig; ++li) {
            const glm::vec3 n =
                RotateBranch(base_n, 1.0f, kGoldenAngle * static_cast<float>(li + 1)); // wide fan
            const glm::vec3 along =
                NormDet(dir) * (len * 0.16f * static_cast<float>(li)); // staggered up the twig
            s.leaves.push_back({tip + along, n});
        }
        return kTwigRadius;
    }
    constexpr float kGoldenAngle = 2.39996323f;
    float sum_r2 = 0.0f;
    for (int i = 0; i < p.child_count; ++i) {
        const float az =
            kGoldenAngle * static_cast<float>(i + 1) + static_cast<float>(depth) * 0.7f;
        glm::vec3 cdir = RotateBranch(dir, p.branch_tilt, az);
        if (p.photo > 0.0f)
            cdir = NormDet(cdir * (1.0f - p.photo) + env.sun_dir * p.photo);
        const float rc = GrowPlant(tip, cdir, len * p.length_ratio, depth + 1, p, env, s);
        sum_r2 += rc * rc;
    }
    const float r = dm::Sqrt(sum_r2); // pipe model
    s.branches.push_back({base, tip, r, depth});
    return r;
}

// PURE: identical (genome, stage, env) -> identical structure on any platform.
inline PlantStructure GeneratePlant(const Comp::PlantGenomeComponent& genome,
                                    std::uint8_t stage,
                                    const PlantEnvDir& env = {}) {
    PlantStructure s;
    const ProcgenParams p = DeriveParams(genome, stage, env);
    // The whole plant leans toward the sun (atmospheric), at half the per-branch strength.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 trunk_dir =
        (p.photo > 0.0f) ? NormDet(up * (1.0f - p.photo * 0.5f) + env.sun_dir * (p.photo * 0.5f))
                         : up;
    GrowPlant(glm::vec3(0.0f), trunk_dir, p.trunk_len, 0, p, env, s);
    return s;
}

// --- Tessellation: PlantStructure -> renderable triangle mesh (the bake core) ---
// What a renderer uploads: branch segments become radial cylinders, leaves become
// camera/sun-facing quads. PURE + deterministic (libm-free trig); the GL upload + scatter
// integration (behind render.plant_procgen) is the render-side follow-on.

struct ProcVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};
struct ProcMesh {
    std::vector<ProcVertex> vertices;
    std::vector<std::uint32_t> indices;
};

inline ProcMesh TessellatePlant(const PlantStructure& s, int radial = 6, float leaf_size = 0.20f) {
    ProcMesh m;
    if (radial < 3)
        radial = 3;

    for (const PlantBranch& br : s.branches) {
        const glm::vec3 axis = br.b - br.a;
        if (axis.x * axis.x + axis.y * axis.y + axis.z * axis.z <= 0.0f)
            continue;
        const glm::vec3 dir = NormDet(axis);
        const glm::vec3 ref = (dir.y > 0.99f || dir.y < -0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                                : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 u = NormDet(CrossDet(dir, ref));
        const glm::vec3 v = CrossDet(dir, u);
        const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
        for (int k = 0; k < radial; ++k) {
            const float ang = dm::kTwoPi * static_cast<float>(k) / static_cast<float>(radial);
            const glm::vec3 off = dm::Cos(ang) * u + dm::Sin(ang) * v; // unit ring offset = normal
            const float uvx = static_cast<float>(k) / static_cast<float>(radial);
            m.vertices.push_back({br.a + off * br.radius, off, glm::vec2(uvx, 0.0f)});
            m.vertices.push_back({br.b + off * br.radius, off, glm::vec2(uvx, 1.0f)});
        }
        for (int k = 0; k < radial; ++k) {
            const int k1 = (k + 1) % radial;
            const std::uint32_t a0 = base + 2u * k, a1 = base + 2u * k + 1u;
            const std::uint32_t b0 = base + 2u * k1, b1 = base + 2u * k1 + 1u;
            m.indices.insert(m.indices.end(), {a0, a1, b1, a0, b1, b0}); // two tris per side
        }
    }

    const float h = leaf_size * 0.5f;
    for (const PlantLeaf& lf : s.leaves) {
        const glm::vec3 n = NormDet(lf.normal);
        const glm::vec3 ref = (n.y > 0.99f || n.y < -0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                            : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 t = NormDet(CrossDet(n, ref));
        const glm::vec3 b = CrossDet(n, t);
        const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
        m.vertices.push_back({lf.pos + (-t - b) * h, n, glm::vec2(0.0f, 0.0f)});
        m.vertices.push_back({lf.pos + (t - b) * h, n, glm::vec2(1.0f, 0.0f)});
        m.vertices.push_back({lf.pos + (t + b) * h, n, glm::vec2(1.0f, 1.0f)});
        m.vertices.push_back({lf.pos + (-t + b) * h, n, glm::vec2(0.0f, 1.0f)});
        m.indices.insert(m.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    }
    return m;
}

} // namespace luminumbra::foliage
