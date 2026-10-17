#pragma once

//  data-driven SPECIES REGISTRY. The engine stays GENERIC — a species is DATA
// (data/common/foliage/species/*.json): a render-archetype string, annual/perennial + lifespan, and
// per-gene [lo,hi] ranges over the existing PlantGene schema. The registry loads those templates
// and can deterministically SAMPLE a PlantGenomeComponent from a species' ranges (seeded RNG), so a
// farm/spawn picks "wheat" or "oak" by id and gets an in-bounds, heritable genome. No
// species-specific logic lives in the engine — only this loader + the data.
//
// DETERMINISM: SampleGenome draws from a caller-supplied seeded DeterministicRng (no wall-clock);
// the sampled genes are clamped to [0,1]. Loading is pure file I/O; the sim/world_hash is
// unaffected until a sampled plant is actually spawned (plants are opt-in via PlantTag).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "../components/PlantComponents.h"
#include "../core/DeterministicRng.h"
#include "../core/FilesystemPath.h"
#include "../world/WorldgenDependencySnapshot.h"

namespace luminumbra::foliage {

namespace Comp = ::Luminumbra::Components;

struct SpeciesTemplate {
    std::string id;
    std::string render_archetype = "generic_plant";
    bool perennial = false;
    std::uint32_t lifespan_ticks = 1200;
    std::array<float, Comp::kPlantGeneCount> gene_lo{}; // per-gene inclusive low
    std::array<float, Comp::kPlantGeneCount> gene_hi{}; // per-gene inclusive high
};

// Map a gene NAME (matching the PlantGene enum) to its index, or -1 if unknown. Engine-generic: the
// JSON authors reference genes by the canonical enum names.
[[nodiscard]] inline int SpeciesGeneIndex(const std::string& name) {
    using G = Comp::PlantGene;
    if (name == "GrowthRate")
        return static_cast<int>(G::GrowthRate);
    if (name == "MaxScale")
        return static_cast<int>(G::MaxScale);
    if (name == "DroughtTolerance")
        return static_cast<int>(G::DroughtTolerance);
    if (name == "ColdTolerance")
        return static_cast<int>(G::ColdTolerance);
    if (name == "HeatTolerance")
        return static_cast<int>(G::HeatTolerance);
    if (name == "LeafDensity")
        return static_cast<int>(G::LeafDensity);
    if (name == "Yield")
        return static_cast<int>(G::Yield);
    if (name == "Hardiness")
        return static_cast<int>(G::Hardiness);
    return -1;
}

inline float SpeciesClamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Stable 16-bit species id derived from the template id string (FNV-1a, folded to 16 bits). Stable
// across file-set changes and runs (NOT a load-order index), so it round-trips through persistence
// (CropLifecycleComponent.species_id / PlantGrowthComponent.species_id) and the plant sub-hash. 0
// is reserved for "unspecified", so a real species never collides with it.
[[nodiscard]] inline std::uint16_t SpeciesId16(const std::string& id) {
    std::uint64_t h = 1469598103934665603ull; // FNV offset basis
    for (unsigned char c : id) {
        h ^= c;
        h *= 1099511628211ull;
    }
    const std::uint16_t v = static_cast<std::uint16_t>((h ^ (h >> 32)) & 0xFFFFu);
    return v == 0 ? 1 : v;
}

// Parse ONE species template from JSON. Genes default to the full [0,1] range when unspecified;
// unknown gene names are ignored. Returns false + an error only on a missing/empty id.
[[nodiscard]] inline bool
ParseSpeciesTemplate(const nlohmann::json& j, SpeciesTemplate& out, std::string& err) {
    out = SpeciesTemplate{};
    out.gene_lo.fill(0.0f);
    out.gene_hi.fill(1.0f);
    if (!j.contains("id") || !j.at("id").is_string() || j.at("id").get<std::string>().empty()) {
        err = "species template missing a non-empty string 'id'";
        return false;
    }
    out.id = j.at("id").get<std::string>();
    if (j.contains("render_archetype") && j.at("render_archetype").is_string())
        out.render_archetype = j.at("render_archetype").get<std::string>();
    if (j.contains("perennial") && j.at("perennial").is_boolean())
        out.perennial = j.at("perennial").get<bool>();
    if (j.contains("lifespan_ticks") && j.at("lifespan_ticks").is_number())
        out.lifespan_ticks = j.at("lifespan_ticks").get<std::uint32_t>();
    if (j.contains("genes") && j.at("genes").is_object()) {
        for (auto it = j.at("genes").begin(); it != j.at("genes").end(); ++it) {
            const int idx = SpeciesGeneIndex(it.key());
            if (idx < 0 || !it.value().is_array() || it.value().size() < 2)
                continue;
            float lo = SpeciesClamp01(it.value()[0].get<float>());
            float hi = SpeciesClamp01(it.value()[1].get<float>());
            if (hi < lo)
                std::swap(lo, hi);
            out.gene_lo[static_cast<std::size_t>(idx)] = lo;
            out.gene_hi[static_cast<std::size_t>(idx)] = hi;
        }
    }
    return true;
}

class SpeciesRegistry {
public:
    // Add a species from raw JSON text (the unit-testable path; no filesystem).
    bool AddFromJsonText(const std::string& text, std::string& err) {
        try {
            SpeciesTemplate t;
            if (!ParseSpeciesTemplate(nlohmann::json::parse(text), t, err))
                return false;
            m_species.push_back(std::move(t));
            return true;
        } catch (const std::exception& e) {
            err = std::string("species JSON parse error: ") + e.what();
            return false;
        }
    }

    // Load immediate regular *.json in UTF-8 filename order. Malformed names and
    // JSON are diagnosed and skipped. Scan/I/O errors set io_failed and leave the
    // registry unchanged so callers can retry without accumulating duplicates.
    // The iterator contract also supports directory providers with virtual paths.
    template<typename DirectoryIterator = std::filesystem::directory_iterator>
    std::size_t LoadFromDirectory(const std::filesystem::path& dir,
                                  std::vector<std::string>& errors,
                                  bool* io_failed = nullptr) {
        if (io_failed != nullptr)
            *io_failed = false;
        const std::size_t original_size = m_species.size();
        const auto fail = [&](const std::string& message) {
            if (io_failed != nullptr)
                *io_failed = true;
            errors.push_back(message);
            m_species.resize(original_size);
        };
        try {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec) || ec) {
                fail("species directory unavailable: " + (ec ? ec.message() : "not a directory"));
                return 0;
            }
            std::vector<std::pair<std::string, std::filesystem::path>> files;
            DirectoryIterator current(dir, ec);
            const DirectoryIterator end;
            if (ec) {
                fail("cannot enumerate species directory: " + ec.message());
                return 0;
            }
            while (current != end) {
                const auto& entry = *current;
                const bool regular = entry.is_regular_file(ec);
                if (ec) {
                    fail("cannot inspect species entry: " + ec.message());
                    return 0;
                }
                if (regular && entry.path().extension() == ".json") {
                    std::string filename;
                    bool converted = false;
                    try {
                        filename = ::Luminumbra::Filesystem::GenericUtf8PathString(
                            entry.path().filename());
                        converted = true;
                    } catch (const std::exception& error) {
                        errors.push_back("species filename UTF-8 conversion failed: " +
                                         std::string(error.what()));
                    }
                    if (converted) {
                        if (::Luminumbra::World::IsValidDependencyPathUtf8(filename))
                            files.emplace_back(std::move(filename), entry.path());
                        else
                            errors.push_back("species filename is not valid UTF-8");
                    }
                }
                current.increment(ec);
                if (ec) {
                    fail("cannot advance species directory: " + ec.message());
                    return 0;
                }
            }
            // Cache converted names: sorting must never perform another conversion.
            std::sort(files.begin(), files.end(), [](const auto& first, const auto& second) {
                return first.first < second.first;
            });
            std::size_t loaded = 0;
            for (const auto& [filename, file] : files) {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    fail("cannot open species file: " + filename);
                    return 0;
                }
                std::string bytes;
                // Bound each read to 8192 bytes; accept content only after clean EOF.
                std::array<char, 8192> buffer{};
                while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
                       in.gcount() > 0)
                    bytes.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
                if (in.bad() || !in.eof()) {
                    fail("cannot read complete species file: " + filename);
                    return 0;
                }
                std::string err;
                if (AddFromJsonText(bytes, err))
                    ++loaded;
                else
                    errors.push_back(filename + ": " + err);
            }
            return loaded;
        } catch (const std::exception& error) {
            fail("species directory load failed: " + std::string(error.what()));
        } catch (...) {
            fail("species directory load failed: unknown exception");
        }
        return 0;
    }

    [[nodiscard]] const SpeciesTemplate* Find(const std::string& id) const {
        for (const auto& s : m_species)
            if (s.id == id)
                return &s;
        return nullptr;
    }
    [[nodiscard]] std::size_t size() const {
        return m_species.size();
    }
    [[nodiscard]] const std::vector<SpeciesTemplate>& all() const {
        return m_species;
    }

    // Deterministically sample a genome from a species' per-gene ranges using a seeded RNG.
    [[nodiscard]] static Comp::PlantGenomeComponent
    SampleGenome(const SpeciesTemplate& t, luminumbra::core::DeterministicRng& rng) {
        Comp::PlantGenomeComponent g;
        for (std::size_t i = 0; i < g.genes.size(); ++i) {
            const float lo = t.gene_lo[i], hi = t.gene_hi[i];
            g.genes[i] = SpeciesClamp01(lo + rng.next_unit() * (hi - lo));
        }
        return g;
    }

private:
    std::vector<SpeciesTemplate> m_species;
};

} // namespace luminumbra::foliage
