#pragma once

//  creature state for the Utility-AI brain (ai/CreatureBrainSystem.h). The presence
// of this component is the per-entity opt-in (like PlantTag): a world with no creatures runs
// the brain system as a no-op, so the canonical headless world_hash stays byte-identical.
// All fields are sim state (integer-ish floats, deterministic); geometry/rendering is separate.

#include <cstddef>
#include <cstdint>

namespace Luminumbra::Components {

// Stable 16-bit creature SPECIES id derived from a species name string (FNV-1a folded
// to 16 bits) — the same scheme foliage uses (foliage::SpeciesId16), kept here as a
// dependency-light free function so the spawn sites and the codex agree on the key.
// 0 is reserved for "unspecified" (a creature with no resolved species), so a named
// species never collides with the unset default. This is IDENTITY metadata: it is set
// once at spawn and never mutated, and is deliberately NOT folded into the ecology
// sub-hash (ai/EcologyHash.h) — adding a constant, never-changing identity tag would
// not alter sim behaviour, and leaving it out keeps every existing determinism baseline
// byte-identical. The PHOTO CODEX (client-only, hash-free) reads it as the species key.
[[nodiscard]] inline std::uint16_t CreatureSpeciesId16(const char* id) {
    std::uint64_t h = 1469598103934665603ull; // FNV offset basis
    for (const char* p = id; p && *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull; // FNV prime
    }
    const std::uint16_t v = static_cast<std::uint16_t>((h ^ (h >> 32)) & 0xFFFFu);
    return v == 0 ? 1 : v;
}

struct CreatureComponent {
    bool is_predator = false; // role: predators hunt prey; prey flee predators + graze
    // Codex species identity (CreatureSpeciesId16 of the archetype/species name). 0 ==
    // unspecified -> the photo codex falls back to the predator/prey role proxy. Set at
    // spawn, inherited by offspring, never mutated; not in the ecology sub-hash.
    std::uint16_t species_id = 0;
    float hunger = 0.0f;  // 0 sated.. 1 starving (grows each tick)
    float stamina = 1.0f; // 0 exhausted.. 1 fresh (short-term exertion; recovers on Rest)
    // long-term sleep/fatigue need. 0 exhausted.. 1 rested. Drains slowly every tick
    // (being awake costs energy), recovers on Rest and fastest on Sleep. Distinct from `stamina`
    // (sprint fuel), energy gates the daily sleep cycle. The brain reads it through
    // CreatureSenses and EcologyHash includes it in authoritative state.
    float energy = 1.0f;
    float move_speed = 3.0f; // m/s cruise
    int last_action = 0;     // last CreatureAction chosen (telemetry / sub-hash)
    // The brain's desired HORIZONTAL velocity (m/s) this tick. When the creature has a
    // CreaturePhysicsComponent the brain writes this and the Jolt character controller owns
    // the resulting position (gravity / terrain collision / slopes); otherwise the brain
    // integrates position directly (the pure, unit-tested path).
    float wish_x = 0.0f;
    float wish_z = 0.0f;
    // Set true when a predator catches this (prey) creature: it becomes an inert carcass
    // (stops deciding/moving) and predators ignore it when seeking live prey.
    bool eaten = false;
};

//  + physics: opt-in TRUE-PHYSICS locomotion. When present, the creature is driven by
// a deterministic Jolt CharacterVirtual (the same controller the player/networked avatars
// use): the brain produces a wish velocity, the avatar resolves it against the terrain
// heightfield (gravity, ground-stick, 50deg max slope), and the resolved position is read
// back into the TransformComponent. avatar_index is the slot in PhysicsSystem's avatar pool.
struct CreaturePhysicsComponent {
    std::size_t avatar_index = 0;
};

// Creature evolution: CREATURE EVOLUTION: the heritable genome carried by a creature plus the
// per-creature reproduction bookkeeping (age + cooldown). Presence of this component is the
// per-entity opt-in for the CreatureReproductionSystem (luminumbra::ai): a world whose
// creatures carry NO genome never reproduces, so the canonical roster stays byte-identical
// (NetworkStateHash baseline green) and existing brain behaviour is unchanged.
//
// The trait fields mirror luminumbra::ai::CreatureGenome (kept here as plain floats so the
// component header has no dependency on the ai/ layer). The reproduction system applies these
// to CreatureComponent (e.g. move_speed) so selection is visible in behaviour. DEFAULTS
// reproduce today's brain constants exactly (move_speed 3.0), so a default-stamped creature
// behaves identically to one with no genome.
struct CreatureGenomeComponent {
    // --- heritable traits (mirror ai::CreatureGenome field order) ---
    float move_speed = 3.0f;       // m/s cruise
    float vigilance = 0.5f;        // flee bias (reserved behaviour hook)
    float hunger_threshold = 0.3f; // reproduce only when hunger <= this
    float size_scale = 1.0f;       // visual/sim size cue
    // ---  SENSORY genes (mirror ai::CreatureGenome; defaults == PerceptionComponent defaults
    // so a default-stamped creature perceives identically to before). Heritable + mutable -> the
    // perceptual phenotype that diverges under selection. ---
    float vision_cos_half_fov = 0.5f; // cos(half FOV); lower = wider cone
    float vision_range = 20.0f;       // m
    float hearing_range = 24.0f;      // m

    // --- reproduction bookkeeping (integer ticks; deterministic) ---
    // Ticks since this creature was born/spawned. A creature must reach maturity before it
    // can reproduce.
    std::uint32_t age_ticks = 0;
    // Ticks remaining before this creature may reproduce again (set after each birth so
    // populations grow at a bounded rate rather than exploding).
    std::uint32_t reproduce_cooldown = 0;
    // Generation index (0 = founder); offspring = max(parents)+1. Telemetry / marker tint.
    std::uint32_t generation = 0;
    // --- SEXUAL reproduction: a creature has a sex and must COURT an opposite-sex mate ---
    // Sex (sexual reproduction needs a male + a female). Assigned at birth by a seeded coin
    // flip; founders are seeded by the spawner.
    bool female = false;
    // Ticks the creature has spent adjacent to a ready opposite-sex mate. When it reaches the
    // courtship duration a single offspring is born. Resets to 0 when no mate is in reach.
    std::uint32_t courting_ticks = 0;
};

} // namespace Luminumbra::Components
