#pragma once

// Pillar A (foliage polish): deterministic PROCEDURAL plant-geometry generator.
// Spec: .forge/specs/foliage-polish/spec.md. PURE function of (genome, growth stage,
// atmosphere) -> branch-skeleton line list. This REPLACES the baked static tree models:
// plants are grown PROGRAMMATICALLY (recursion), GENETICALLY (genome drives structure),
// and ATMOSPHERICALLY (branches bend toward the sun — phototropism — and could respond
// to light/wind), so two genomes grow structurally different plants, a plant elaborates
// as it matures, and a plant leans toward its light.
//
// DETERMINISM (docs/STANDARDS.md §4/§9): VISUAL-ONLY — never feeds the sim, never
// touches world_hash. But it is a deterministic pure function so a baked cache is
// reproducible and a future CPU/GPU split agrees. Cross-platform bit-stable: uses
// ONLY Luminumbra::DeterministicMath (libm-free Sin/Cos/Sqrt) + IEEE basic ops, with
// hand-rolled normalize/cross (NOT glm::normalize/glm::rotate, which are libm-backed
// and platform-divergent).

#include "../components/PlantComponents.h"
#include "../core/DeterministicMath.h"
#include "PlantGrowthSystem.h"  // ExpressGenome / PlantPhenotype

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
    if (len2 <= 0.0f) return glm::vec3(0.0f);
    const float inv = 1.0f / dm::Sqrt(len2);  // IEEE-deterministic sqrt
    return glm::vec3(v.x * inv, v.y * inv, v.z * inv);
}

// Rotate a unit direction by (tilt about a perpendicular, azimuth around `dir`).
inline glm::vec3 RotateBranch(const glm::vec3& dir, float tilt, float az) {
    // Stable orthonormal frame (u, v) perpendicular to dir: avoid the degenerate
    // near-vertical case by switching the reference axis.
    const glm::vec3 ref = (dir.y > 0.99f || dir.y < -0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                            : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 u = NormDet(CrossDet(dir, ref));
    const glm::vec3 v = CrossDet(dir, u);  // unit (dir,u orthonormal)
    const float ct = dm::Cos(tilt), st = dm::Sin(tilt);
    const float ca = dm::Cos(az), sa = dm::Sin(az);
    const glm::vec3 child = ct * dir + st * (ca * u + sa * v);
    return NormDet(child);
}

// ATMOSPHERIC growth inputs. The renderer supplies these from the live sky/weather
// (real tick: sun direction from the day/night cycle, light from season/exposure).
// Defaults (sun straight up, no phototropism) reproduce the pure genetic form.
struct PlantEnvDir {
    glm::vec3 sun_dir = glm::vec3(0.0f, 1.0f, 0.0f);  // UNIT direction TO the sun
    float phototropism = 0.0f;                        // [0,1] strength of sun-seeking
};

struct ProcgenParams {
    float trunk_len = 1.0f;
    int child_count = 2;     // branches per node (2..3)
    float branch_tilt = 0.6f;  // radians off the parent axis
    float length_ratio = 0.7f; // child length / parent length
    int max_depth = 0;       // recursion depth (from growth stage)
    float photo = 0.0f;      // resolved phototropism strength (atmosphere x genome)
};

inline ProcgenParams DeriveParams(const Comp::PlantGenomeComponent& g, std::uint8_t stage,
                                  const PlantEnvDir& env) {
    using G = Comp::PlantGene;
    const PlantPhenotype ph = ExpressGenome(g);
    ProcgenParams p;
    p.trunk_len = ph.max_scale;                                  // 0.6..2.4
    p.child_count = 2 + (g.gene(G::LeafDensity) > 0.5f ? 1 : 0); // 2 or 3
    p.branch_tilt = 0.45f + g.gene(G::GrowthRate) * 0.50f;       // ~0.45..0.95 rad
    p.length_ratio = 0.62f + g.gene(G::MaxScale) * 0.18f;        // 0.62..0.80
    // Stage -> recursion depth: Seed(0) = trunk only; deeper as it matures, capped.
    p.max_depth = stage > 5 ? 5 : static_cast<int>(stage);
    // ATMOSPHERIC x GENETIC: leafier genomes seek light harder. clamp01 keeps it [0,1].
    p.photo = clamp01(env.phototropism * (0.3f + 0.5f * ph.leaf_density));
    return p;
}

inline void GrowBranch(const glm::vec3& base, const glm::vec3& dir, float len, int depth,
                       const ProcgenParams& p, const PlantEnvDir& env,
                       std::vector<glm::vec3>& out) {
    const glm::vec3 tip = base + dir * len;
    out.push_back(base);
    out.push_back(tip);  // one skeleton segment (GL line list)
    if (depth >= p.max_depth) return;
    constexpr float kGoldenAngle = 2.39996323f;  // radians (137.5 deg) — phyllotaxis spread
    for (int i = 0; i < p.child_count; ++i) {
        const float az = kGoldenAngle * static_cast<float>(i + 1) + static_cast<float>(depth) * 0.7f;
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
    glm::vec3 a;   // base
    glm::vec3 b;   // tip
    float radius;  // pipe-model cross-section radius of this segment
    int depth;
};
struct PlantLeaf {
    glm::vec3 pos;     // attach point (a terminal twig tip)
    glm::vec3 normal;  // facing direction (toward the sun under phototropism)
};
struct PlantStructure {
    std::vector<PlantBranch> branches;
    std::vector<PlantLeaf> leaves;
};

inline constexpr float kTwigRadius = 0.01f;  // terminal-twig base radius (pipe-model leaf unit)

// Post-order recursion: returns this branch's pipe-model radius. Da Vinci / Borchert-Honda
// pipe model — a parent's cross-section area equals the sum of its children's
// (r_parent = sqrt(Sum r_child^2)) -> a naturally thick trunk tapering to thin twigs.
inline float GrowPlant(const glm::vec3& base, const glm::vec3& dir, float len, int depth,
                       const ProcgenParams& p, const PlantEnvDir& env, PlantStructure& s) {
    const glm::vec3 tip = base + dir * len;
    if (depth >= p.max_depth) {
        s.branches.push_back({base, tip, kTwigRadius, depth});
        const glm::vec3 leaf_n = (p.photo > 0.0f) ? NormDet(env.sun_dir) : dir;  // leaves face the light
        s.leaves.push_back({tip, leaf_n});
        return kTwigRadius;
    }
    constexpr float kGoldenAngle = 2.39996323f;
    float sum_r2 = 0.0f;
    for (int i = 0; i < p.child_count; ++i) {
        const float az = kGoldenAngle * static_cast<float>(i + 1) + static_cast<float>(depth) * 0.7f;
        glm::vec3 cdir = RotateBranch(dir, p.branch_tilt, az);
        if (p.photo > 0.0f) cdir = NormDet(cdir * (1.0f - p.photo) + env.sun_dir * p.photo);
        const float rc = GrowPlant(tip, cdir, len * p.length_ratio, depth + 1, p, env, s);
        sum_r2 += rc * rc;
    }
    const float r = dm::Sqrt(sum_r2);  // pipe model
    s.branches.push_back({base, tip, r, depth});
    return r;
}

// PURE: identical (genome, stage, env) -> identical structure on any platform.
inline PlantStructure GeneratePlant(const Comp::PlantGenomeComponent& genome, std::uint8_t stage,
                                    const PlantEnvDir& env = {}) {
    PlantStructure s;
    const ProcgenParams p = DeriveParams(genome, stage, env);
    // The whole plant leans toward the sun (atmospheric), at half the per-branch strength.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 trunk_dir =
        (p.photo > 0.0f) ? NormDet(up * (1.0f - p.photo * 0.5f) + env.sun_dir * (p.photo * 0.5f)) : up;
    GrowPlant(glm::vec3(0.0f), trunk_dir, p.trunk_len, 0, p, env, s);
    return s;
}

}  // namespace luminumbra::foliage
