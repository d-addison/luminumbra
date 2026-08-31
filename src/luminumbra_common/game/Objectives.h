#pragma once

// game.objectives: the photography PROGRESSION layer that sits on top of the codex
// and gives the player a REASON to photograph: a set of objectives ("discover N species",
// "get a 4-star shot of the flagship creature", "build a collection worth X") each evaluated
// as a PURE function of the PhotoCodex state. Completing them is what turns "walk around
// and take pictures" into a game with goals.
//
// SCOPE. NO render, NO GL, NO entt, NO rng, NO wall-clock, NO global state — exactly the
// pure-value discipline PhotoCodex/PhotoSession follow, so it is unit-testable headlessly
// and the determinism fixture can pin it. It reads a codex (and, for star objectives, the
// codex's recorded best score) and reports completion + a [0,1] progress fraction. It
// stores no codex of its own and mutates nothing.
//
// DETERMINISM. Evaluation is float +-*/ and integer compares plus the existing libm-free
// StarsForTotal step function; the same codex always yields the same statuses (run==replay).
// Objectives carry a stable id-sorted order so iteration is reproducible.

#include <cstdint>
#include <string>
#include <vector>

#include "PhotoCodex.h"   // PhotoCodex / CodexEntry
#include "PhotoSession.h" // StarsForTotal (0..5 from a [0,1] total)

namespace luminumbra::game {

// What an objective measures. Kept small + explicit so evaluation is a pure switch.
enum class ObjectiveKind : std::uint8_t {
    DiscoverCount = 0, // discover at least `target_count` distinct species
    DiscoverSpecies,   // discover the species `species_id`
    StarRating,        // own a >= `min_stars` shot of species `species_id`
    CollectionScore,   // reach a total collection score of `min_score`
    BehavioralMatch,   // capture a subject performing `target_action` (a brain action
                       // value; if `species_id` > 0, of that species — else any species)
};

// One objective. `title` is the player-facing line; the params are interpreted per kind.
struct Objective {
    int id = 0; // stable ordering / save key
    ObjectiveKind kind = ObjectiveKind::DiscoverCount;
    std::string title;
    int target_count = 0;   // DiscoverCount
    int species_id = 0;     // DiscoverSpecies / StarRating / BehavioralMatch (0 = any)
    int min_stars = 0;      // StarRating (0..5)
    float min_score = 0.0f; // CollectionScore
    int target_action = -1; // BehavioralMatch: brain action value (<0 = unset)
};

// The evaluated state of one objective against a codex.
struct ObjectiveStatus {
    bool complete = false;
    float progress = 0.0f; // [0,1] — fraction of the way to completion
};

// Pure clamp (mirrors the libraries' helper; float +-*/ only).
inline float ObjectiveClamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

// Best recorded score for a species, or 0 if undiscovered. Linear scan over the codex's
// id-sorted entries (small N) — pure, order-stable.
inline float CodexBestScore(const PhotoCodex& codex, int species_id) {
    for (const CodexEntry& e : codex.entries()) {
        if (e.species_id == species_id)
            return e.best_score;
    }
    return 0.0f;
}

// Evaluate ONE objective against the codex. Deterministic.
inline ObjectiveStatus EvaluateObjective(const Objective& o, const PhotoCodex& codex) {
    ObjectiveStatus s;
    switch (o.kind) {
        case ObjectiveKind::DiscoverCount: {
            const int have = static_cast<int>(codex.species_count());
            const int need = o.target_count > 0 ? o.target_count : 1;
            s.complete = have >= need;
            s.progress = ObjectiveClamp01(static_cast<float>(have) / static_cast<float>(need));
            break;
        }
        case ObjectiveKind::DiscoverSpecies: {
            s.complete = codex.discovered(o.species_id);
            s.progress = s.complete ? 1.0f : 0.0f;
            break;
        }
        case ObjectiveKind::StarRating: {
            const int stars = codex.discovered(o.species_id)
                                  ? StarsForTotal(CodexBestScore(codex, o.species_id))
                                  : 0;
            const int need = o.min_stars > 0 ? o.min_stars : 1;
            s.complete = stars >= need;
            s.progress = ObjectiveClamp01(static_cast<float>(stars) / static_cast<float>(need));
            break;
        }
        case ObjectiveKind::CollectionScore: {
            const float have = codex.total_score();
            const float need = o.min_score > 0.0f ? o.min_score : 1.0f;
            s.complete = have >= need;
            s.progress = ObjectiveClamp01(have / need);
            break;
        }
        case ObjectiveKind::BehavioralMatch: {
            // Complete once a capture of the target behaviour exists — of the named
            // species (species_id > 0) or of ANY species (species_id <= 0). Binary:
            // you have either photographed the behaviour or you have not.
            const bool got = o.species_id > 0
                                 ? codex.behavior_captured(o.species_id, o.target_action)
                                 : codex.behavior_captured_any(o.target_action);
            s.complete = got;
            s.progress = got ? 1.0f : 0.0f;
            break;
        }
    }
    return s;
}

// A set of objectives + aggregate queries. Pure: evaluation never mutates the set or the
// codex; it just reports against a codex snapshot.
class ObjectiveSet {
public:
    void Add(const Objective& o) {
        objectives_.push_back(o);
    }

    [[nodiscard]] const std::vector<Objective>& all() const {
        return objectives_;
    }
    [[nodiscard]] std::size_t size() const {
        return objectives_.size();
    }

    // How many objectives are complete against `codex`.
    [[nodiscard]] std::uint32_t completed_count(const PhotoCodex& codex) const {
        std::uint32_t n = 0;
        for (const Objective& o : objectives_) {
            if (EvaluateObjective(o, codex).complete)
                ++n;
        }
        return n;
    }

    // True once every objective is complete (the "you finished the starter goals" gate).
    [[nodiscard]] bool all_complete(const PhotoCodex& codex) const {
        for (const Objective& o : objectives_) {
            if (!EvaluateObjective(o, codex).complete)
                return false;
        }
        return !objectives_.empty();
    }

    // The first incomplete objective (the one to surface as "current"), or nullptr when
    // all are done / the set is empty.
    [[nodiscard]] const Objective* next_incomplete(const PhotoCodex& codex) const {
        for (const Objective& o : objectives_) {
            if (!EvaluateObjective(o, codex).complete)
                return &o;
        }
        return nullptr;
    }

private:
    std::vector<Objective> objectives_;
};

// The starter objective set — the first goals a new player chases. Stable order/ids so
// the HUD + save are reproducible. `first_species_id` keys the species-specific goals on
// the first creature the world is known to spawn (e.g. the flagship herbivore) so the starter
// goals are always achievable in the default world.
inline ObjectiveSet DefaultObjectives(int first_species_id) {
    ObjectiveSet set;
    Objective o0;
    o0.id = 0;
    o0.kind = ObjectiveKind::DiscoverSpecies;
    o0.species_id = first_species_id;
    o0.title = "Photograph your first creature";
    set.Add(o0);

    Objective o1;
    o1.id = 1;
    o1.kind = ObjectiveKind::DiscoverCount;
    o1.target_count = 3;
    o1.title = "Discover 3 species";
    set.Add(o1);

    Objective o2;
    o2.id = 2;
    o2.kind = ObjectiveKind::StarRating;
    o2.species_id = first_species_id;
    o2.min_stars = 4;
    o2.title = "Take a 4-star portrait";
    set.Add(o2);

    Objective o3;
    o3.id = 3;
    o3.kind = ObjectiveKind::DiscoverCount;
    o3.target_count = 6;
    o3.title = "Fill the codex (6 species)";
    set.Add(o3);

    Objective o4;
    o4.id = 4;
    o4.kind = ObjectiveKind::CollectionScore;
    o4.min_score = 3.0f;
    o4.title = "Build a collection worth 3.0";
    set.Add(o4);

    // Behaviour-driven goal: catch a creature in its daily life, not just present it.
    // target_action 5 == the brain's Sleep action (CreatureBrain CreatureAction::Sleep);
    // species_id 0 == any species. Satisfied by photographing a sleeping (rest-posed)
    // creature — the living-world depth the codex now rewards.
    Objective o5;
    o5.id = 5;
    o5.kind = ObjectiveKind::BehavioralMatch;
    o5.target_action = 5;
    o5.species_id = 0;
    o5.title = "Photograph a sleeping creature";
    set.Add(o5);
    return set;
}

} // namespace luminumbra::game
