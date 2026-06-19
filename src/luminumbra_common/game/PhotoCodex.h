#pragma once

// Track game.photo_codex — the creature/subject CODEX: a PURE, DETERMINISTIC
// record of which species the player has captured (photographed) plus a score of
// the collection. This is the pillar-G progression layer that sits ON TOP of the
// PhotoScoring rubric: the camera produces a PhotoShot, PhotoScoring grades it into
// a PhotoScore, and the resulting per-capture score is logged here against the
// subject's species so the Codex can answer "what have I discovered?", "how good is
// my best shot of each?", and "how complete is my collection?".
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng, NO wall-clock. It is a plain
// value container with deterministic accessors so it can be unit-tested in isolation
// and later fed by whatever capture pipeline the game grows. Keeping the collection
// model here, pure and dependency-free, lets the progression loop be tuned + reasoned
// about without dragging in the renderer.
//
// DETERMINISM CONTRACT. Entries are kept in a STABLE species_id-sorted order at all
// times (insertion does an ordered insert), so iteration via entries() is byte-for-
// byte reproducible regardless of capture order — recording the same set of captures
// in any order yields the same entries() vector and the same aggregate scores
// (run==replay). All arithmetic is float +-*/ only (no libm transcendentals); the
// only comparison is best-score max. There is NO rng (this track has no seed offset),
// NO global state. Every method is a pure function of the recorded captures.
//
// GATING. A fresh, empty Codex is a DEFINED zero state: species_count()==0,
// total_score()==0, completeness(n)==0, entries() empty. The Codex never touches an
// entt registry or any baseline hash, so it cannot perturb the NetworkStateHash.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// CodexEntry. One discovered species: its id, how many times it has been
// captured, and the BEST capture score seen for it (from PhotoScoring's
// PhotoScore::total, but the Codex stores only the plain float so it stays free of
// any scorer dependency).
// ---------------------------------------------------------------------------
struct CodexEntry {
    int      species_id = 0;
    std::uint32_t captures = 0;
    float    best_score = 0.0f;
};

// ---------------------------------------------------------------------------
// Small pure helper — float +-*/ only, no libm.
// ---------------------------------------------------------------------------
inline float CodexClamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// ---------------------------------------------------------------------------
// PhotoCodex. The collection. Entries are maintained sorted ascending by
// species_id so iteration is deterministic and discovered() can binary-search.
// ---------------------------------------------------------------------------
class PhotoCodex {
public:
    // Log a capture of `species_id` scored `photo_score` (typically a
    // PhotoScore::total in [0,1]). First sighting of a species DISCOVERS it
    // (captures=1, best_score=photo_score); a repeat sighting increments captures
    // and keeps the BEST (max) score — a later, worse shot never lowers the record.
    // Insertion preserves the species_id-sorted invariant.
    void Record(int species_id, float photo_score) {
        const std::size_t idx = LowerBound(species_id);
        if (idx < entries_.size() && entries_[idx].species_id == species_id) {
            // Existing species: another capture, keep the best score.
            ++entries_[idx].captures;
            if (photo_score > entries_[idx].best_score) {
                entries_[idx].best_score = photo_score;
            }
            return;
        }
        // New species: insert in sorted position to preserve the ordering invariant.
        CodexEntry e;
        e.species_id = species_id;
        e.captures = 1;
        e.best_score = photo_score;
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
        if (total_species <= 0) return 0.0f;
        const float frac = static_cast<float>(entries_.size()) /
                           static_cast<float>(total_species);
        return CodexClamp01(frac);
    }

    // The discovered entries, ALWAYS sorted ascending by species_id (deterministic
    // iteration order).
    const std::vector<CodexEntry>& entries() const { return entries_; }

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
