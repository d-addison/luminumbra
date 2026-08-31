#pragma once

// Heritable creature genomes and deterministic inheritance operators. A genome
// is a small fixed set of real-valued traits that drive
// the creature's behaviour (move_speed) and reproduction eligibility (hunger threshold,
// vigilance/flee bias) plus a visual/sim size cue. Selection becomes VISIBLE because:
//   * caught prey die (CreatureComponent.eaten) and never pass on their genes, and
//   * well-fed, healthy, mature prey reproduce (CreatureReproductionSystem) and pass a
//     mutated copy of their genome to ONE offspring.
//
// Determinism: every stochastic
// draw comes from a caller-supplied seeded DeterministicRng (splitmix64; NO wall-clock /
// std::random), and the Gaussian mutation is the libm-free Irwin-Hall one from
// DeterministicRng. Same seed + same parent(s) -> byte-identical offspring genome
// (run==replay). A creature without a genome retains the default behavior.
//
// The per-gene Gaussian mutation, blend crossover, and gene clamping come from
// ai/Evolution.h; this header adds named creature traits and canonical bounds.

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
    float vigilance = 0.5f;        // 0 oblivious.. 1 paranoid (flee bias; reserved hook)
    float hunger_threshold = 0.3f; // reproduce only when hunger <= this (well-fed gate)
    float size_scale = 1.0f;       // visual/sim size cue (>1 bigger); inherited + mutated
    // ---  SENSORY genes (the perceptual phenotype). Defaults reproduce the engine's hard-coded
    // PerceptionComponent / HearingProfile defaults EXACTLY, so a default-genome creature perceives
    // identically to before this slice. Heritable + mutable -> predator/prey SENSORY divergence
    // under selection (narrow far-seeing predator cone vs wide near hearing-led prey). Carried
    // OUTSIDE the 4-gene core GA vector so the existing breeding RNG stream (and the ecology hash)
    // is untouched; inherited by BreedSensoryInto from draws taken AFTER the core breed + sex draw.
    // ---
    float vision_cos_half_fov = 0.5f; // cos(half FOV); lower = WIDER cone, higher = narrower
    float vision_range = 20.0f;       // m (matches PerceptionComponent.vision_range)
    float hearing_range = 24.0f;      // m (matches HearingProfile.range)
};

// Number of genes in the flat vector encoding (mirrors CreatureGenome's fields, in order).
inline constexpr std::size_t kCreatureGeneCount = 4;

// Canonical inclusive bounds for each gene (clamp after mutation/crossover). Move speed is
// kept in a sane locomotion band; the others in their natural [0,1]/(0,inf-ish] ranges.
[[nodiscard]] inline std::array<GeneBound, kCreatureGeneCount> CreatureGeneBounds() {
    return {GeneBound{1.0f, 8.0f},  // move_speed
            GeneBound{0.0f, 1.0f},  // vigilance
            GeneBound{0.05f, 0.6f}, // hunger_threshold
            GeneBound{0.6f, 1.8f}}; // size_scale
}

// Flatten a genome to the gene vector (field order == bounds order).
[[nodiscard]] inline std::vector<float> CreatureGenomeToGenes(const CreatureGenome& g) {
    return {g.move_speed, g.vigilance, g.hunger_threshold, g.size_scale};
}

// Rebuild a genome from a gene vector (missing genes keep the default).
[[nodiscard]] inline CreatureGenome CreatureGenomeFromGenes(const std::vector<float>& v) {
    CreatureGenome g;
    if (v.size() > 0)
        g.move_speed = v[0];
    if (v.size() > 1)
        g.vigilance = v[1];
    if (v.size() > 2)
        g.hunger_threshold = v[2];
    if (v.size() > 3)
        g.size_scale = v[3];
    return g;
}

// Default mutation step as a FRACTION of each gene's range (Evolution.h convention). 8% is
// the GA-typical small step that drifts traits without scrambling them generation to gen.
inline constexpr float kCreatureMutationSigmaFrac = 0.08f;

// ---  SENSORY gene count + canonical bounds (declared here, ahead of the breeding
// operators, so SpeciesGenomeRanges below can default from them; the inheritance operator
// BreedSensoryInto stays in the sensory section at the bottom of this header). ---
inline constexpr std::size_t kCreatureSensoryGeneCount = 3;

// Canonical inclusive bounds: a wide cone (cos ~0.2 ~= 156 deg) for prey down to a narrow cone
// (cos ~0.95 ~= 36 deg) for a focused predator; vision/hearing ranges in a sane metre band.
[[nodiscard]] inline std::array<GeneBound, kCreatureSensoryGeneCount> CreatureSensoryGeneBounds() {
    return {GeneBound{0.2f, 0.95f},  // vision_cos_half_fov
            GeneBound{6.0f, 45.0f},  // vision_range (m)
            GeneBound{6.0f, 45.0f}}; // hearing_range (m)
}

// per-species GENOME-RANGE overrides. The defaults ARE the canonical
// bounds (CreatureGeneBounds / CreatureSensoryGeneBounds — single source of truth, no literal
// duplication), so a default-constructed SpeciesGenomeRanges clamps mutation/crossover exactly
// as before -> byte-identical breeding. Species JSON (key "genome_ranges") narrows/widens the
// bands per species (e.g. a slow heavy grazer vs a fast light darter) via
// CreatureSpeciesRegistry; index order == the gene-vector encodings in this header.
struct SpeciesGenomeRanges {
    // core[i] bounds CreatureGenomeToGenes order: move_speed, vigilance, hunger_threshold,
    // size_scale.
    std::array<GeneBound, kCreatureGeneCount> core = CreatureGeneBounds();
    // sensory[i] bounds CreatureSensoryToGenes order: vision_cos_half_fov, vision_range,
    // hearing_range.
    std::array<GeneBound, kCreatureSensoryGeneCount> sensory = CreatureSensoryGeneBounds();
};

// Produce an offspring genome from ONE parent (asexual: mutate a copy). Deterministic for a
// given rng state. Reuses Evolution.h GaussianMutate + clamp. `ranges` defaults to the
// canonical bounds -> byte-identical to the historical single-argument behaviour.
[[nodiscard]] inline CreatureGenome MutateOffspring(const CreatureGenome& parent,
                                                    luminumbra::core::DeterministicRng& rng,
                                                    const SpeciesGenomeRanges& ranges = {}) {
    std::vector<GeneBound> bv(ranges.core.begin(), ranges.core.end());
    std::vector<float> genes = CreatureGenomeToGenes(parent);
    GaussianMutate(genes, bv, kCreatureMutationSigmaFrac, rng);
    return CreatureGenomeFromGenes(genes);
}

// Produce an offspring genome from TWO parents (sexual: blend-crossover then mutate).
// Deterministic for a given rng state. Reuses Evolution.h BlendCrossover + GaussianMutate.
// `ranges` defaults to the canonical bounds -> byte-identical to the historical behaviour.
[[nodiscard]] inline CreatureGenome BreedOffspring(const CreatureGenome& a,
                                                   const CreatureGenome& b,
                                                   luminumbra::core::DeterministicRng& rng,
                                                   const SpeciesGenomeRanges& ranges = {}) {
    std::vector<GeneBound> bv(ranges.core.begin(), ranges.core.end());
    std::vector<float> child =
        BlendCrossover(CreatureGenomeToGenes(a), CreatureGenomeToGenes(b), rng);
    GaussianMutate(child, bv, kCreatureMutationSigmaFrac, rng);
    return CreatureGenomeFromGenes(child);
}

// ---  SENSORY genes: kept separate from the 4-gene core so the existing breeding RNG stream is
// byte-identical. Inheritance draws are taken AFTER BreedOffspring + the sex draw (see
// CreatureReproductionSystem), so the core genome and child sex are unaffected. The count and
// canonical bounds are declared ABOVE (before SpeciesGenomeRanges). ---
[[nodiscard]] inline std::vector<float> CreatureSensoryToGenes(const CreatureGenome& g) {
    return {g.vision_cos_half_fov, g.vision_range, g.hearing_range};
}

inline void ApplySensoryGenes(CreatureGenome& g, const std::vector<float>& v) {
    if (v.size() > 0)
        g.vision_cos_half_fov = v[0];
    if (v.size() > 1)
        g.vision_range = v[1];
    if (v.size() > 2)
        g.hearing_range = v[2];
}

// Inherit the SENSORY genes (sexual blend-crossover + mutate) into an already-bred `child`, using
// rng draws taken AFTER the core BreedOffspring + sex draw. Returns the child with its sensory
// genes set; the core genome fields are left untouched. Deterministic for a given rng state.
// `ranges` defaults to the canonical bounds -> byte-identical to the historical behaviour.
[[nodiscard]] inline CreatureGenome BreedSensoryInto(CreatureGenome child,
                                                     const CreatureGenome& a,
                                                     const CreatureGenome& b,
                                                     luminumbra::core::DeterministicRng& rng,
                                                     const SpeciesGenomeRanges& ranges = {}) {
    std::vector<GeneBound> bv(ranges.sensory.begin(), ranges.sensory.end());
    std::vector<float> genes =
        BlendCrossover(CreatureSensoryToGenes(a), CreatureSensoryToGenes(b), rng);
    GaussianMutate(genes, bv, kCreatureMutationSigmaFrac, rng);
    ApplySensoryGenes(child, genes);
    return child;
}

} // namespace luminumbra::ai
