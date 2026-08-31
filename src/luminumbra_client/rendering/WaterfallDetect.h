#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

// ===========================================================================
// waterfall SITE DETECTION.  but WORLD-DETERMINISTIC.
//
// A waterfall is a rendered phenomenon (no new sim — documented design): the
// dressing (the falling sheet shader, the  spray particles, the plunge-pool
// foam, the distance-attenuated roar) is render-only and is NEVER hashed. But
// the SITES the dressing attaches to must be the SAME for every player and
// every replay (regression review): detection is a PURE FUNCTION of the generated
// world data — the  river course (RiverInfluenceAt) crossed with the
// heightfield (GetTerrainHeightAt) — computed once per region and CACHED,
// independent of camera or frame. The same seed yields byte-identical sites.
//
// ONE-WAY RULE (regression review / documented design): this reads world-gen queries only
// and writes nothing back into any sim/world_hash input. world_hash stays
// d950a6afc12a5cdc.
//
// DETECTION ALGORITHM (pure, deterministic):
//   For each cell on a fixed lattice that carries river influence (the river
//   course), look DOWNSTREAM along the local height gradient over a short span:
//   where the river surface descends a STEEP drop (the per-run fall exceeds a
//   drop threshold over a short downstream run, i.e. a near-cliff in the
//   channel floor), that cell is a waterfall LIP. Lips are then de-duplicated
//   into discrete SITES (one site per local cluster, the cluster's steepest lip
//   wins) so a single fall is one site, not a smear of adjacent lip cells. The
//   crest, the drop height, the channel width and the downhill flow azimuth are
//   all read from the heightfield — no RNG, no global seed taken.
//
//   The detection lattice + thresholds are FIXED constants below, so the site
//   SET is a stable function of (world seed, terrain params). The DetectionKey
//   (seed + the analysis window) keys the per-world cache.
// ===========================================================================

struct WaterfallSite {
    // World-space crest (lip) of the fall: the river column at the top of the
    // drop, y = the terrain surface height there.
    glm::vec3 crest{0.0f};
    // World-space foot of the fall: the plunge point at the bottom of the drop
    // (the lip advanced downstream by the detected run, y = the lower surface).
    glm::vec3 foot{0.0f};
    // Total vertical drop in metres (crest.y - foot.y, > 0).
    float drop_height = 0.0f;
    // Horizontal channel run the drop spans (metres) — the sheet length.
    float run_length = 0.0f;
    // Estimated channel width at the lip (metres) from the river-influence band.
    float width = 0.0f;
    // Unit downhill flow azimuth in the XZ plane (crest -> foot direction).
    glm::vec2 flow_dir{0.0f, 0.0f};
    // Mean slope across the drop (drop_height / run_length), the "steepness".
    float steepness = 0.0f;
    // Creation source: false = river-course lip (steep drop in the river channel);
    // true = perched-lake/tarn OUTLET (a rim dropoff where the lake spills over).
    // Diagnostic only (NOT hashed) — both kinds bake the same sheet dressing.
    bool lake_outlet = false;
};

// Fixed detection parameters (PINNED — changing these changes the site set and
// is a deliberate gate update the baseline, exactly like the river PV band). Defaults are
// tuned for the shipped mountains preset (rivers enabled, channel carved toward
// SEA_LEVEL through sloped terrain).
struct WaterfallDetectParams {
    float lattice_step = 4.0f;     // sample spacing (m); matches the river atlas step
    float min_drop = 3.0f;         // minimum vertical drop (m) to count as a fall
    float min_steepness = 0.45f;   // minimum drop/run slope (a near-cliff in the channel)
    float max_run = 16.0f;         // longest downstream run scanned for a drop (m)
    float river_threshold = 0.05f; // RiverInfluenceAt above this == on the river course
    float cluster_radius = 24.0f;  // de-dup radius (m): lips within this collapse to one site
    int half_extent = 768;         // analysis window half-extent (m) around the origin
    // --- Lake / tarn OUTLET detection ---
    // A perched lake (WaterLevelAt > SEA_LEVEL) that spills over its rim down a
    // slope creates a waterfall at the rim. A lake cell qualifies as an outlet
    // crest when an OUTSIDE neighbour (not itself in the lake) has terrain that
    // falls >= lake_outlet_min_drop below the lake surface within a short
    // downhill run. Tuned so only true rim dropoffs qualify, not gentle shores.
    float lake_surface_epsilon =
        0.25f; // WaterLevelAt must exceed SEA_LEVEL by this to be "in a lake"
    float lake_outlet_min_drop = 3.0f;       // min terrain fall below lake surface at the rim (m)
    float lake_outlet_min_steepness = 0.40f; // min drop/run slope of the rim dropoff
    float lake_outlet_max_run = 20.0f;       // longest downhill run scanned past the rim (m)
};

// Stable, hashable key for the per-world site cache: the world seed plus the
// (quantized) detection window. Two SHIELD_WorldSystems built from the same
// seed/params produce the same key -> the same cached site set.
struct WaterfallDetectKey {
    int seed = 0;
    int half_extent = 0;
    int lattice_step_milli = 0;
    int min_drop_milli = 0;
    int min_steepness_milli = 0;
    // terraform bed edits advance the sim's water epoch; folding it
    // here makes a dammed/dug river trigger ONE bounded re-survey (a new cache
    // entry) instead of per-frame re-detection or a stale-forever site set.
    std::uint64_t water_epoch = 0;

    bool operator==(const WaterfallDetectKey& o) const {
        return seed == o.seed && half_extent == o.half_extent &&
               lattice_step_milli == o.lattice_step_milli && min_drop_milli == o.min_drop_milli &&
               min_steepness_milli == o.min_steepness_milli && water_epoch == o.water_epoch;
    }
};

// Pure, deterministic detection over the world's river+height fields. Returns
// the sites in a STABLE order (sorted by a deterministic spatial key), so two
// calls on the same world produce byte-identical vectors. No caching here — the
// raw scan; callers that want caching use WaterfallSiteCache below.
std::vector<WaterfallSite> DetectWaterfalls(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                            const WaterfallDetectParams& params = {});

// FNV-1a hash of the quantized site fields, order-preserving. The determinism
// surface the WaterfallVisual gate asserts (same seed -> same hash).
uint64_t HashWaterfallSites(const std::vector<WaterfallSite>& sites);

// the LIVE upstream water factor for a site, [0,1]. Reads the live
// water surface at the CREST (the render float mirror, one-way derived from the
// mm truth — legal after derived-state reclassification): 1 = a healthy sheet (water depth at the
// crest
// >= full_depth), 0 = upstream dammed/drained (the sheet extinguishes). Returns
// 1 (neutral) when the crest's chunk carries no live grid — an UNSTREAMED site
// is unknown, not extinguished. Pure; render-only.
float LiveWaterFactorAt(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                        const WaterfallSite& site,
                        float full_depth = 0.25f);

// Per-world cache: detection is computed once per (seed, window) and reused for
// every subsequent query (camera/frame independent — regression review). Render-side,
// owned by the RenderPipeline; never hashed.
class WaterfallSiteCache {
public:
    // Returns the cached site set for `world`, computing+caching it on first use.
    const std::vector<WaterfallSite>&
    sites_for(const Luminumbra::Systems::SHIELD_WorldSystem& world,
              const WaterfallDetectParams& params = {});

    void clear() {
        m_cache.clear();
    }
    std::size_t cached_world_count() const {
        return m_cache.size();
    }

private:
    struct KeyHash {
        std::size_t operator()(const WaterfallDetectKey& k) const noexcept;
    };
    std::unordered_map<WaterfallDetectKey, std::vector<WaterfallSite>, KeyHash> m_cache;
};

// Builds the cache key for a world + params (PUBLIC so the gate can assert the
// key derivation is a pure function of the seed/window).
WaterfallDetectKey MakeWaterfallDetectKey(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                          const WaterfallDetectParams& params);

} // namespace Luminumbra::Rendering
