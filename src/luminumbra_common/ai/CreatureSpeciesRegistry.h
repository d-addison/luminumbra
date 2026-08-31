#pragma once

// data-driven CREATURE SPECIES REGISTRY. Mirrors foliage::SpeciesRegistry:
// the engine stays GENERIC and a creature species is DATA (data/common/creatures/
// species/*.json) — a stable name (-> CreatureSpeciesId16), a player-facing display
// name, a predator/prey role, a rarity weight, and a base body colour. The codex/HUD
// resolve a captured species_id to its display name + rarity through this registry, and
// procedural-creature appearance seeds its recolor from base_color and morphs
// from the genome ranges this registry will grow.
//
// DETERMINISM: loading is pure file I/O and computes ids via the same FNV-1a name hash
// the spawn sites use (Components::CreatureSpeciesId16). LoadFromDirectory sorts the
// directory entries by filename before parsing, so the loaded table is a PURE FUNCTION of
// the file CONTENT — never of filesystem iteration order. The registry itself touches no
// entt registry and no world_hash — it is content metadata the client + spawners read;
// the per-species behaviour blocks (: "brain" IAUS overrides +
// "genome_ranges") default to the compiled constants, so a world with NO species JSON —
// or JSON that omits/matches the defaults — resolves a table byte-identical to the
// compiled-in behaviour. Any FUTURE field the sim consumes as an integer (tick counts,
// counts) must be parsed as an integer (get<std::uint32_t>), never via a float round-trip.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "../components/CreatureComponents.h"
#include "CreatureBrain.h"  // CreatureBrainParams (per-species IAUS overrides)
#include "CreatureGenome.h" // SpeciesGenomeRanges (per-species gene bounds)

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;

// One creature species template. `id` is the stable name (the FNV key the spawners and
// codex agree on); everything else is presentation/content metadata.
struct CreatureSpecies {
    std::string id;           // stable species name, e.g. "deer"
    std::string display_name; // player-facing, e.g. "Deer"
    bool predator = false;    // role default (prey unless stated)
    bool nocturnal = false;   // active by night (sleeps by day); data-driven so a
                              // new species can be nocturnal without a client recompile
    float rarity = 0.5f;      // [0,1] discovery prestige (codex/scoring hook)
    float base_color[3] = {0.45f, 0.42f, 0.38f}; //  recolor seed (linear RGB)
    // Biomes this species inhabits (by biome name, e.g. "wetland"). EMPTY = lives
    // anywhere (a generalist), so undated/legacy data still spawns everywhere.
    std::vector<std::string> biomes;

    // ---: per-species BEHAVIOUR data. Both blocks default to the
    // compiled constants (CreatureBrainParams{} == the DecideCreatureAction literals;
    // SpeciesGenomeRanges{} == CreatureGeneBounds+CreatureSensoryGeneBounds), so a
    // species JSON that omits them — and every legacy/shipped file — loads a table entry
    // byte-identical to the compiled-in behaviour. ---
    CreatureBrainParams brain;         // IAUS action weights + flee curve shape
    SpeciesGenomeRanges genome_ranges; // mutation/crossover clamp bands per gene

    // The codex/spawn key — FNV-1a of `id`, identical to the value spawn sites stamp
    // onto CreatureComponent::species_id.
    [[nodiscard]] std::uint16_t species_id() const {
        return Comp::CreatureSpeciesId16(id.c_str());
    }
    // Does this species live in `biome`? A generalist (empty list) lives everywhere.
    [[nodiscard]] bool lives_in(const std::string& biome) const {
        if (biomes.empty())
            return true;
        for (const std::string& b : biomes)
            if (b == biome)
                return true;
        return false;
    }
};

// Parse ONE species from JSON. Only a non-empty string `id` is required; everything else
// defaults. `display_name` falls back to the id when absent.
[[nodiscard]] inline bool
ParseCreatureSpecies(const nlohmann::json& j, CreatureSpecies& out, std::string& err) {
    out = CreatureSpecies{};
    if (!j.contains("id") || !j.at("id").is_string() || j.at("id").get<std::string>().empty()) {
        err = "creature species missing a non-empty string 'id'";
        return false;
    }
    out.id = j.at("id").get<std::string>();
    out.display_name = out.id;
    if (j.contains("display_name") && j.at("display_name").is_string())
        out.display_name = j.at("display_name").get<std::string>();
    if (j.contains("predator") && j.at("predator").is_boolean())
        out.predator = j.at("predator").get<bool>();
    if (j.contains("nocturnal") && j.at("nocturnal").is_boolean())
        out.nocturnal = j.at("nocturnal").get<bool>();
    if (j.contains("rarity") && j.at("rarity").is_number()) {
        float r = j.at("rarity").get<float>();
        out.rarity = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    }
    if (j.contains("base_color") && j.at("base_color").is_array() &&
        j.at("base_color").size() >= 3) {
        for (int i = 0; i < 3; ++i)
            out.base_color[i] = j.at("base_color")[i].get<float>();
    }
    if (j.contains("biomes") && j.at("biomes").is_array()) {
        for (const auto& b : j.at("biomes"))
            if (b.is_string())
                out.biomes.push_back(b.get<std::string>());
    }

    // ---: optional per-species behaviour overrides. Lenient like the
    // fields above: only well-typed entries apply; anything absent/malformed keeps the
    // compiled default, so a partial override is safe and an empty/absent block is
    // byte-identical to no JSON at all. ---
    if (j.contains("brain") && j.at("brain").is_object()) {
        const nlohmann::json& b = j.at("brain");
        auto num = [&b](const char* key, float fallback) -> float {
            return (b.contains(key) && b.at(key).is_number()) ? b.at(key).get<float>() : fallback;
        };
        out.brain.wander_weight = num("wander_weight", out.brain.wander_weight);
        out.brain.rest_weight = num("rest_weight", out.brain.rest_weight);
        out.brain.sleep_weight = num("sleep_weight", out.brain.sleep_weight);
        out.brain.hunt_weight = num("hunt_weight", out.brain.hunt_weight);
        out.brain.flee_weight = num("flee_weight", out.brain.flee_weight);
        out.brain.graze_weight = num("graze_weight", out.brain.graze_weight);
        out.brain.flee_logistic_m = num("flee_logistic_m", out.brain.flee_logistic_m);
        out.brain.flee_logistic_c = num("flee_logistic_c", out.brain.flee_logistic_c);
    }
    if (j.contains("genome_ranges") && j.at("genome_ranges").is_object()) {
        const nlohmann::json& g = j.at("genome_ranges");
        // A range entry is a 2-element numeric [lo, hi] array with lo <= hi; anything else
        // (wrong type, wrong arity, inverted band) is ignored and the canonical bound holds.
        auto bound = [&g](const char* key, GeneBound fallback) -> GeneBound {
            if (!g.contains(key) || !g.at(key).is_array() || g.at(key).size() != 2)
                return fallback;
            const nlohmann::json& r = g.at(key);
            if (!r[0].is_number() || !r[1].is_number())
                return fallback;
            const float lo = r[0].get<float>();
            const float hi = r[1].get<float>();
            if (!(lo <= hi))
                return fallback; // also rejects NaN
            return GeneBound{lo, hi};
        };
        // Key -> index map mirrors CreatureGenomeToGenes / CreatureSensoryToGenes order.
        out.genome_ranges.core[0] = bound("move_speed", out.genome_ranges.core[0]);
        out.genome_ranges.core[1] = bound("vigilance", out.genome_ranges.core[1]);
        out.genome_ranges.core[2] = bound("hunger_threshold", out.genome_ranges.core[2]);
        out.genome_ranges.core[3] = bound("size_scale", out.genome_ranges.core[3]);
        out.genome_ranges.sensory[0] = bound("vision_cos_half_fov", out.genome_ranges.sensory[0]);
        out.genome_ranges.sensory[1] = bound("vision_range", out.genome_ranges.sensory[1]);
        out.genome_ranges.sensory[2] = bound("hearing_range", out.genome_ranges.sensory[2]);
    }
    return true;
}

class CreatureSpeciesRegistry {
public:
    // Add a species from raw JSON text (the unit-testable path; no filesystem).
    bool AddFromJsonText(const std::string& text, std::string& err) {
        try {
            CreatureSpecies s;
            if (!ParseCreatureSpecies(nlohmann::json::parse(text), s, err))
                return false;
            m_species.push_back(std::move(s));
            return true;
        } catch (const std::exception& e) {
            err = std::string("creature species JSON parse error: ") + e.what();
            return false;
        }
    }

    // Load every *.json under `dir` (sorted by filename for determinism). Returns the
    // count loaded; per-file failures are appended to `errors` and skipped.
    std::size_t LoadFromDirectory(const std::filesystem::path& dir,
                                  std::vector<std::string>& errors) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            errors.push_back("creature species directory not found: " + dir.string());
            return 0;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        std::size_t loaded = 0;
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            if (!in) {
                errors.push_back("cannot open " + f.string());
                continue;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string err;
            if (AddFromJsonText(ss.str(), err))
                ++loaded;
            else
                errors.push_back(f.filename().string() + ": " + err);
        }
        return loaded;
    }

    // Resolve by stable id-16 (the value carried on CreatureComponent::species_id).
    [[nodiscard]] const CreatureSpecies* Find(std::uint16_t id16) const {
        for (const auto& s : m_species)
            if (s.species_id() == id16)
                return &s;
        return nullptr;
    }
    [[nodiscard]] const CreatureSpecies* FindByName(const std::string& id) const {
        for (const auto& s : m_species)
            if (s.id == id)
                return &s;
        return nullptr;
    }
    // Display name for a captured species id, or a stable "Species #<id>" fallback for an
    // unregistered creature so the UI never shows a blank.
    [[nodiscard]] std::string DisplayName(std::uint16_t id16) const {
        if (const CreatureSpecies* s = Find(id16))
            return s->display_name;
        return "Species #" + std::to_string(static_cast<unsigned>(id16));
    }

    [[nodiscard]] std::size_t size() const {
        return m_species.size();
    }
    [[nodiscard]] const std::vector<CreatureSpecies>& all() const {
        return m_species;
    }

    // The species that inhabit `biome` (generalists included), in registry order.
    [[nodiscard]] std::vector<const CreatureSpecies*>
    SpeciesInBiome(const std::string& biome) const {
        std::vector<const CreatureSpecies*> out;
        for (const auto& s : m_species)
            if (s.lives_in(biome))
                out.push_back(&s);
        return out;
    }

    // Deterministically pick the `pick`-th species (modulo count) that inhabits `biome`.
    // Returns nullptr only when NO species (not even a generalist) matches — callers fall
    // back to the full roster. Pure: `pick` selects, no rng.
    [[nodiscard]] const CreatureSpecies* SelectForBiome(const std::string& biome,
                                                        std::size_t pick) const {
        const std::vector<const CreatureSpecies*> in = SpeciesInBiome(biome);
        if (in.empty())
            return nullptr;
        return in[pick % in.size()];
    }

private:
    std::vector<CreatureSpecies> m_species;
};

} // namespace luminumbra::ai
