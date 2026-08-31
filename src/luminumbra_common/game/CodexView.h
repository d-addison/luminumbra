#pragma once

// game.codex_view: the PURE presentation model for the codex browse screen. It
// joins the data-driven species registry (every species the world CAN show) with the
// player's PhotoCodex (what they have discovered + their best shot) into a deterministic,
// id-sorted list of rows the UI renders. Keeping this pure (no GL, no entt, no rng) means
// the codex screen's CONTENT — which species appear, locked vs discovered, the star
// rating, and the completion fraction — is unit-testable without a renderer.
//
// DETERMINISM. Rows are emitted in ascending species_id order (stable, registry/codex
// order-independent); stars come from the libm-free StarsForTotal; completion is a plain
// float ratio. The same (registry, codex) always yields the same view.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../ai/CreatureSpeciesRegistry.h"
#include "PhotoCodex.h"
#include "PhotoSession.h"  // StarsForTotal

namespace luminumbra::game {

// One row of the codex screen: a species, whether the player has discovered it, and (if
// so) the best-shot star rating + capture count.
struct CodexRow {
    std::uint16_t species_id = 0;
    std::string   display_name;
    bool          discovered = false;
    int           stars = 0;          // 0..5 from the best recorded shot (0 if locked)
    std::uint32_t captures = 0;
    float         rarity = 0.5f;
};

// The whole screen model: every registered species as a row, plus the aggregate counts.
struct CodexView {
    std::vector<CodexRow> rows;
    std::uint32_t discovered_count = 0;
    std::size_t   total_species = 0;
    float         completeness = 0.0f;  // discovered / total, [0,1]
};

// Build the codex view: one row per REGISTERED species (so undiscovered species show as
// locked targets), joined with the player's codex for discovered/stars/captures. Rows are
// sorted ascending by species_id for deterministic display.
inline CodexView BuildCodexView(const luminumbra::ai::CreatureSpeciesRegistry& registry,
                                const PhotoCodex& codex) {
    CodexView view;
    view.total_species = registry.size();

    for (const auto& species : registry.all()) {
        CodexRow row;
        row.species_id = species.species_id();
        row.display_name = species.display_name;
        row.rarity = species.rarity;
        row.discovered = codex.discovered(static_cast<int>(row.species_id));
        if (row.discovered) {
            for (const CodexEntry& e : codex.entries()) {
                if (e.species_id == static_cast<int>(row.species_id)) {
                    row.stars = StarsForTotal(e.best_score);
                    row.captures = e.captures;
                    break;
                }
            }
            ++view.discovered_count;
        }
        view.rows.push_back(row);
    }

    std::sort(view.rows.begin(), view.rows.end(),
              [](const CodexRow& a, const CodexRow& b) { return a.species_id < b.species_id; });

    view.completeness = codex.completeness(static_cast<int>(view.total_species));
    return view;
}

}  // namespace luminumbra::game
