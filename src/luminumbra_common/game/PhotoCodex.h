#pragma once

// game.photo_codex: the creature/subject CODEX: a PURE, DETERMINISTIC
// record of which species the player has captured (photographed) plus a score of
// the collection. This is the photography progression layer that sits ON TOP of the
// PhotoScoring rubric: the camera produces a PhotoShot, PhotoScoring grades it into
// a PhotoScore, and the resulting per-capture score is logged here against the
// subject's species so the Codex can answer "what have I discovered?", "how good is
// my best shot of each?", and "how complete is my collection?".
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng, NO wall-clock. It is a plain
// value container with deterministic accessors so it can be unit-tested in isolation
// and fed by the runtime capture pipeline. Keeping the collection
// model here, pure and dependency-free, lets the progression loop be tuned + reasoned
// about without dragging in the renderer.
//
// DETERMINISM CONTRACT. Entries are kept in a STABLE species_id-sorted order at all
// times (insertion does an ordered insert), so iteration via entries is byte-for-
// byte reproducible regardless of capture order — recording the same set of captures
// in any order yields the same entries vector and the same aggregate scores
// (run==replay). All arithmetic is float +-*/ only (no libm transcendentals); the
// only comparison is best-score max. There is NO rng (this track has no seed offset),
// NO global state. Every method is a pure function of the recorded captures.
//
// GATING. A fresh, empty Codex is a DEFINED zero state: species_count==0,
// total_score==0, completeness(n)==0, entries empty. The Codex never touches an
// entt registry or any baseline hash, so it cannot perturb the NetworkStateHash.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "GameMath.h" // luminumbra::game: Clamp01

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// ObservationMetadata. The behavioural/temporal context a capture was taken IN,
// recorded ALONGSIDE the verdict so the progression layer can pose behaviour-driven
// goals ("photograph a SLEEPING creature", "shoot the colony at dusk"). It is plain
// data, NOT a scoring input — EvaluateShot never reads it; it annotates the capture.
//
// `subject_action` is the principal subject's behaviour at the instant of capture,
// carried as a plain int so this pure header stays DECOUPLED from the ai/ creature
// brain (the client maps its CreatureAction enum onto this int; the convention is
// the brain's enum value, e.g. Sleep). A negative value means "no behaviour" (an
// inanimate / unknown subject); such captures set no behaviour bit in the codex.
// `time_of_day` and `scene_luminance` are [0,1] (dawn=0..dusk).
// ---------------------------------------------------------------------------
struct ObservationMetadata {
    int subject_action = -1;      // principal subject behaviour (brain enum value; <0 = none)
    float time_of_day = 0.5f;     // [0,1] day-clock phase at capture
    float scene_luminance = 0.5f; // [0,1] ambient brightness at capture
};

// Behaviour bit for a brain action value. action<0 (no behaviour) -> 0 (no bit). The
// codex ORs these into a per-species mask so "have I captured action N?" is a pure
// bit test. Actions >= 32 are out of the mask's range and contribute no bit (defined,
// not UB) — the brain's action set is small (< 32) so this is a backstop, not a limit.
inline std::uint32_t BehaviorBit(int action) {
    if (action < 0 || action >= 32)
        return 0u;
    return 1u << static_cast<unsigned>(action);
}

// ---------------------------------------------------------------------------
// CodexEntry. One discovered species: its id, how many times it has been
// captured, the BEST capture score seen for it (from PhotoScoring's
// PhotoScore::total, but the Codex stores only the plain float so it stays free of
// any scorer dependency), and a BEHAVIOUR MASK — the OR of every captured subject
// action's BehaviorBit (which behaviours of this species the player has photographed).
// ---------------------------------------------------------------------------
struct CodexEntry {
    int species_id = 0;
    std::uint32_t captures = 0;
    float best_score = 0.0f;
    std::uint32_t behavior_mask = 0u; // OR of BehaviorBit(action) over captures
};

// ---------------------------------------------------------------------------
// Small pure helper — float +-*/ only, no libm.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// PhotoCodex. The collection. Entries are maintained sorted ascending by
// species_id so iteration is deterministic and discovered can binary-search.
// ---------------------------------------------------------------------------
class PhotoCodex {
public:
    // Log a capture of `species_id` scored `photo_score` (typically a
    // PhotoScore::total in [0,1]). First sighting of a species DISCOVERS it
    // (captures=1, best_score=photo_score); a repeat sighting increments captures
    // and keeps the BEST (max) score — a later, worse shot never lowers the record.
    // Insertion preserves the species_id-sorted invariant.
    //
    // `subject_action` (default -1 = none) records the behaviour the subject was in:
    // its BehaviorBit is OR'd into the species' behaviour_mask so behaviour-driven
    // objectives can later ask "have I captured this species ASLEEP?". A behaviour bit
    // is monotonic (once captured it stays set), matching best_score's keep-the-best
    // discipline. Order-independent: ORing bits and max-ing scores both commute, so the
    // codex stays run==replay regardless of capture order.
    void Record(int species_id, float photo_score, int subject_action = -1) {
        const std::uint32_t bit = BehaviorBit(subject_action);
        const std::size_t idx = LowerBound(species_id);
        if (idx < entries_.size() && entries_[idx].species_id == species_id) {
            // Existing species: another capture, keep the best score, accumulate behaviour.
            ++entries_[idx].captures;
            if (photo_score > entries_[idx].best_score) {
                entries_[idx].best_score = photo_score;
            }
            entries_[idx].behavior_mask |= bit;
            return;
        }
        // New species: insert in sorted position to preserve the ordering invariant.
        CodexEntry e;
        e.species_id = species_id;
        e.captures = 1;
        e.best_score = photo_score;
        e.behavior_mask = bit;
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(idx), e);
    }

    // Has this species been captured at least once?
    bool discovered(int species_id) const {
        const std::size_t idx = LowerBound(species_id);
        return idx < entries_.size() && entries_[idx].species_id == species_id;
    }

    // Number of DISTINCT species discovered.
    std::uint32_t species_count() const {
        return static_cast<std::uint32_t>(entries_.size());
    }

    // Sum of the BEST score across every discovered species (the collection's
    // aggregate quality). Summed in species_id order so the float total is
    // reproducible. Empty codex -> 0.
    float total_score() const {
        float acc = 0.0f;
        for (const auto& e : entries_) {
            acc += e.best_score;
        }
        return acc;
    }

    // Fraction of the world's species the player has discovered, clamped to [0,1].
    // total_species <= 0 is treated as 0 completeness (an undefined denominator
    // yields the defined zero rather than a divide-by-zero).
    float completeness(int total_species) const {
        if (total_species <= 0)
            return 0.0f;
        const float frac = static_cast<float>(entries_.size()) / static_cast<float>(total_species);
        return Clamp01(frac);
    }

    // Has the player captured species `species_id` performing brain action `action`?
    // Pure bit test against that species' accumulated behaviour mask; false if the
    // species is undiscovered or the action was never photographed.
    bool behavior_captured(int species_id, int action) const {
        const std::uint32_t bit = BehaviorBit(action);
        if (bit == 0u)
            return false;
        const std::size_t idx = LowerBound(species_id);
        if (idx >= entries_.size() || entries_[idx].species_id != species_id)
            return false;
        return (entries_[idx].behavior_mask & bit) != 0u;
    }

    // Has the player captured ANY species performing brain action `action`? OR of every
    // entry's mask, in species_id order (order-stable). Used by species-agnostic
    // behaviour objectives ("photograph a sleeping creature", any species).
    bool behavior_captured_any(int action) const {
        const std::uint32_t bit = BehaviorBit(action);
        if (bit == 0u)
            return false;
        for (const auto& e : entries_) {
            if ((e.behavior_mask & bit) != 0u)
                return true;
        }
        return false;
    }

    // The discovered entries, ALWAYS sorted ascending by species_id (deterministic
    // iteration order).
    const std::vector<CodexEntry>& entries() const {
        return entries_;
    }

private:
    // First index whose species_id is >= `species_id` (std::lower_bound over the
    // sorted vector). Pure: only integer comparisons. Used for both lookup and the
    // sorted-insert position so the ordering invariant is maintained.
    std::size_t LowerBound(int species_id) const {
        std::size_t lo = 0;
        std::size_t hi = entries_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (entries_[mid].species_id < species_id) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    std::vector<CodexEntry> entries_;
};

} // namespace luminumbra::game
