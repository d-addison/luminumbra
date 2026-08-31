#pragma once

// game.codex_entry: the per-SPECIES metadata layer of the photography codex
// (photography). Where PhotoCodex records what the player has CAPTURED (per-species
// captures + best score), SpeciesRegistry describes the species THEMSELVES: a stable,
// PURE catalogue of which species exist in the world, how rare each one is, what
// habitat it favours, and whether it is nocturnal. It also supplies the RARITY WEIGHT
// that the scoring layer multiplies in when grading a capture — rarer subjects are
// worth more, so the two pieces compose (PhotoCodex = the record, SpeciesRegistry =
// the description + rarity multiplier).
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng, NO wall-clock. It is a plain
// value container with deterministic accessors so it can be unit-tested in isolation
// and shared by both the scorer and any UI that wants to show "what is this creature?".
//
// DETERMINISM CONTRACT. Entries are kept in a STABLE species_id-sorted order at all
// times (Register does an ordered insert / in-place update), so iteration via all is
// byte-for-byte reproducible regardless of registration order — registering the same
// set of species in any order yields the same all vector (run==replay). All
// arithmetic is float +-*/ only (no libm transcendentals); the only comparisons are
// integer species_id compares. There is NO rng (this track has no seed offset), NO
// global mutable state. Every method is a pure function of the registered species.
//
// GATING. A fresh, empty registry is a DEFINED zero state: size==0, all empty,
// Find(anything)==nullptr, RarityWeight(anything)==0. The registry never touches an
// entt registry or any baseline hash, so it cannot perturb the NetworkStateHash.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// SpeciesInfo. One catalogued species: its id, display name, a rarity tier
// (0 = common, larger = rarer), a habitat tag, and whether it is nocturnal.
// `name` is a borrowed string literal (the registry stores the pointer verbatim);
// callers are expected to pass static/long-lived strings (as the codex content
// tables do). Plain value type — no ownership, no allocation.
// ---------------------------------------------------------------------------
struct SpeciesInfo {
    int species_id = 0;
    const char* name = "";
    std::uint8_t rarity = 0;  // 0 = common; higher = rarer.
    std::uint8_t habitat = 0; // game-defined habitat tag.
    bool nocturnal = false;
};

// ---------------------------------------------------------------------------
// Rarity -> weight slope. RarityWeight returns 1 + rarity * kRarityWeightSlope
// so a common species (rarity 0) is worth the baseline 1.0 and each rarity tier
// adds a fixed increment. Pure float multiply/add — no libm.
// ---------------------------------------------------------------------------
inline constexpr float kRarityWeightSlope = 0.25f;

// ---------------------------------------------------------------------------
// SpeciesRegistry. The species catalogue. Entries are maintained sorted ascending
// by species_id so iteration is deterministic and Find can binary-search.
// ---------------------------------------------------------------------------
class SpeciesRegistry {
public:
    // Catalogue `info`. If the species_id is new it is inserted in sorted position
    // (preserving the ordering invariant); if it already exists the existing entry is
    // UPDATED in place (re-registering a species id overwrites its metadata — defined
    // behaviour, not a duplicate). size therefore counts DISTINCT species ids.
    void Register(const SpeciesInfo& info) {
        const std::size_t idx = LowerBound(info.species_id);
        if (idx < entries_.size() && entries_[idx].species_id == info.species_id) {
            entries_[idx] = info; // re-register updates in place.
            return;
        }
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(idx), info);
    }

    // The catalogued species_id's entry, or nullptr if unknown. Pure lookup over the
    // sorted vector (binary search), no mutation.
    const SpeciesInfo* Find(int species_id) const {
        const std::size_t idx = LowerBound(species_id);
        if (idx < entries_.size() && entries_[idx].species_id == species_id) {
            return &entries_[idx];
        }
        return nullptr;
    }

    // Number of DISTINCT catalogued species.
    std::size_t size() const {
        return entries_.size();
    }

    // Rarity-derived scoring weight for `species_id`: 1 + rarity * slope so a rarer
    // species yields a HIGHER weight. An UNKNOWN species yields 0 (it contributes no
    // weight rather than the baseline 1.0 — the caller can distinguish "not in the
    // catalogue" from "common"). Pure float +*; no libm, no rng.
    float RarityWeight(int species_id) const {
        const SpeciesInfo* info = Find(species_id);
        if (info == nullptr)
            return 0.0f;
        return 1.0f + static_cast<float>(info->rarity) * kRarityWeightSlope;
    }

    // The catalogued species, ALWAYS sorted ascending by species_id (deterministic
    // iteration order).
    const std::vector<SpeciesInfo>& all() const {
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

    std::vector<SpeciesInfo> entries_;
};

// ---------------------------------------------------------------------------
// Optional convenience: a small default catalogue of example species. Pure and
// deterministic — registering through Register keeps the result species_id-sorted
// regardless of the listing order below. Provided so callers/tests have a ready
// baseline; the game is free to build its own registry instead.
// ---------------------------------------------------------------------------
inline SpeciesRegistry MakeDefaultSpeciesRegistry() {
    SpeciesRegistry reg;
    // habitat tags are game-defined opaque ids here (e.g. 0=meadow,1=forest,2=cavern,
    // 3=alpine). Listed deliberately out of id order to exercise the sorted insert.
    reg.Register(SpeciesInfo{3, "Dusk Moth", 2, 1, /*nocturnal*/ true});
    reg.Register(SpeciesInfo{1, "Meadow Sparrow", 0, 0, /*nocturnal*/ false});
    reg.Register(SpeciesInfo{5, "Cavern Wisp", 3, 2, /*nocturnal*/ true});
    reg.Register(SpeciesInfo{2, "Forest Hare", 1, 1, /*nocturnal*/ false});
    reg.Register(SpeciesInfo{4, "Alpine Stag", 2, 3, /*nocturnal*/ false});
    return reg;
}

} // namespace luminumbra::game
