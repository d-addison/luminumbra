#pragma once

// Track (a) — CREATURE EVOLUTION: the heritable creature GENOME and its deterministic
// inheritance operators. A genome is a small fixed set of real-valued traits that drive
// the creature's behaviour (move_speed) and reproduction eligibility (hunger threshold,
// vigilance/flee bias) plus a visual/sim size cue. Selection becomes VISIBLE because:
//   * caught prey die (CreatureComponent.eaten) and never pass on their genes, and
//   * well-fed, healthy, mature prey reproduce (CreatureReproductionSystem) and pass a
//     mutated copy of their genome to ONE offspring.
//
// DETERMINISM (this is on the sim path -> feeds world_hash once wired): every stochastic
// draw comes from a caller-supplied seeded DeterministicRng (splitmix64; NO wall-clock /
// std::random), and the Gaussian mutation is the libm-free Irwin-Hall one from
// DeterministicRng. Same seed + same parent(s) -> byte-identical offspring genome
// (run==replay). The DEFAULT genome reproduces today's brain constants exactly, so a
// creature WITHOUT a genome behaves identically to before this pillar landed.
//
// REUSE: the per-gene Gaussian mutation + blend crossover + gene clamping come from
// ai/Evolution.h (the plant pillar's GA core); this header just gives them a typed,
// creature-specific face (named traits + canonical bounds).

#include <array>
#include <cstddef>
#include <vector>

#include "Evolution.h"

#include "../core/DeterministicRng.h"

namespace luminumbra::ai {

// Heritable creature traits. A pure value type (trivially copyable). The DEFAULTS are
// chosen so a creature stamped with a default genome behaves exactly like one with no
// genome at all (move_speed 3.0 = the brain's historical prey cruise; the other fields
// reproduce the brain's hard-coded reproduction-neutral behaviour).
struct CreatureGenome {
    float move_speed = 3.0f;       // m/s cruise (was CreatureComponent's literal default)
    float vigilance = 0.5f;        // 0 oblivious .. 1 paranoid (flee bias; reserved hook)
    float hunger_threshold = 0.3f; // reproduce only when hunger <= this (well-fed gate)
    float size_scale = 1.0f;       // visual/sim size cue (>1 bigger); inherited + mutated
};

// Number of genes in the flat vector encoding (mirrors CreatureGenome's fields, in order).
inline constexpr std::size_t kCreatureGeneCount = 4;

// Canonical inclusive bounds for each gene (clamp after mutation/crossover). Move speed is
// kept in a sane locomotion band; the others in their natural [0,1]/(0,inf-ish] ranges.
[[nodiscard]] inline std::array<GeneBound, kCreatureGeneCount> CreatureGeneBounds() {
    return {GeneBound{1.0f, 8.0f},   // move_speed
            GeneBound{0.0f, 1.0f},   // vigilance
            GeneBound{0.05f, 0.6f},  // hunger_threshold
            GeneBound{0.6f, 1.8f}};  // size_scale
}

// Flatten a genome to the gene vector (field order == bounds order).
[[nodiscard]] inline std::vector<float> CreatureGenomeToGenes(const CreatureGenome& g) {
    return {g.move_speed, g.vigilance, g.hunger_threshold, g.size_scale};
}

// Rebuild a genome from a gene vector (missing genes keep the default).
[[nodiscard]] inline CreatureGenome CreatureGenomeFromGenes(const std::vector<float>& v) {
    CreatureGenome g;
    if (v.size() > 0) g.move_speed = v[0];
    if (v.size() > 1) g.vigilance = v[1];
    if (v.size() > 2) g.hunger_threshold = v[2];
    if (v.size() > 3) g.size_scale = v[3];
    return g;
}

// Default mutation step as a FRACTION of each gene's range (Evolution.h convention). 8% is
// the GA-typical small step that drifts traits without scrambling them generation to gen.
inline constexpr float kCreatureMutationSigmaFrac = 0.08f;

// Produce an offspring genome from ONE parent (asexual: mutate a copy). Deterministic for a
// given rng state. Reuses Evolution.h GaussianMutate + clamp.
[[nodiscard]] inline CreatureGenome MutateOffspring(const CreatureGenome& parent,
                                                    luminumbra::core::DeterministicRng& rng) {
    const auto bounds = CreatureGeneBounds();
    std::vector<GeneBound> bv(bounds.begin(), bounds.end());
    std::vector<float> genes = CreatureGenomeToGenes(parent);
    GaussianMutate(genes, bv, kCreatureMutationSigmaFrac, rng);
    return CreatureGenomeFromGenes(genes);
}

// Produce an offspring genome from TWO parents (sexual: blend-crossover then mutate).
// Deterministic for a given rng state. Reuses Evolution.h BlendCrossover + GaussianMutate.
[[nodiscard]] inline CreatureGenome BreedOffspring(const CreatureGenome& a, const CreatureGenome& b,
                                                   luminumbra::core::DeterministicRng& rng) {
    const auto bounds = CreatureGeneBounds();
    std::vector<GeneBound> bv(bounds.begin(), bounds.end());
    std::vector<float> child = BlendCrossover(CreatureGenomeToGenes(a), CreatureGenomeToGenes(b), rng);
    GaussianMutate(child, bv, kCreatureMutationSigmaFrac, rng);
    return CreatureGenomeFromGenes(child);
}

}  // namespace luminumbra::ai
