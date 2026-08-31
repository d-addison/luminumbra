#pragma once

// Simulation-side state for living-world foliage.
//
// Procedural plant geometry is visual-only (client/render, baked per genome and
// stage); simulation truth is this compact deterministic state. Growth advances on the
// fixed 30 Hz tick (catch-up via now-last); the genome is the heritable unit the
// farming/breeding loop crosses. Nothing here touches the renderer.
//
// The genome reuses the creature GA's convention (normalized [0,1] real genes in
// ai/Evolution.h), including blend crossover and Gaussian mutation
// operators. The phenotype expansion + growth tick live in
// systems/PlantGrowthSystem.h.

#include <array>
#include <cstdint>

namespace Luminumbra::Components {

// Fixed gene schema — breeding operates gene-for-gene, so the order is stable.
// All genes are normalized [0,1]; PlantGrowthSystem::ExpressGenome maps them to
// real growth parameters (and later, G x E reaction norms).
enum class PlantGene : std::uint8_t {
    GrowthRate = 0,   // accumulation speed (seed -> mature)
    MaxScale,         // mature visual size
    DroughtTolerance, // resistance to low-moisture stress
    ColdTolerance,    // resistance to low-temperature stress
    HeatTolerance,    // resistance to high-temperature stress
    LeafDensity,      // canopy fullness (visual + photosynthesis proxy)
    Yield,            // fruit/crop yield at harvest (farming)
    Hardiness,        // overall stress resistance (raises every floor)
    Count
};
inline constexpr std::size_t kPlantGeneCount = static_cast<std::size_t>(PlantGene::Count);

// The heritable genome. POD; one array so it copies/hashes trivially.
struct PlantGenomeComponent {
    std::array<float, kPlantGeneCount> genes{}; // normalized [0,1]
    float gene(PlantGene g) const {
        return genes[static_cast<std::size_t>(g)];
    }
};

// Discrete growth stages. The renderer picks the baked mesh for the current
// stage; the sim only ever stores the integer index.
enum class PlantStage : std::uint8_t {
    Seed = 0,
    Sprout,
    Juvenile,
    Mature,
    Flowering,
    Fruiting,
    Count
};
inline constexpr std::uint8_t kPlantStageCount = static_cast<std::uint8_t>(PlantStage::Count);

// The plant's DETERMINISTIC growth STATE — replay-exact and cheap to hash.
// growth_points / stress_points are FIXED-POINT milli-units (integer), so no
// float accumulation enters the sim-truth state. quality is the DST-style
// inverse-of-stress harvest score.
struct PlantGrowthComponent {
    std::uint16_t species_id = 0;    // species (base params + which visual model)
    std::uint8_t stage = 0;          // PlantStage index
    std::uint8_t quality = 100;      // 0..100 harvest quality (100 = unstressed)
    std::uint32_t growth_points = 0; // fixed-point accumulated growth (milli-units)
    std::uint32_t stress_points = 0; // fixed-point accumulated stress (milli-units)
    std::uint8_t tended = 0;         // farming husbandry: water/fertilize bonus that
                                     // boosts suitability + eases stress, decays each
                                     // tick (so the player must keep tending).
    std::uint64_t planted_tick = 0;  // tick planted (catch-up reference)
    std::uint64_t last_tick = 0;     // last update tick (catch-up via now - last)
};

// Opt-in marker. ONLY entities tagged with this participate in plant growth, so
// the canonical headless roster (which has no plants) stays byte-identical and
// the world_hash baseline is unperturbed until plants are deliberately spawned.
struct PlantTag {};

} // namespace Luminumbra::Components
