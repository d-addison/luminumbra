#include "WaterfallDetect.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Luminumbra::Rendering {

namespace {

// Stable quantization helpers (mm grid) so the determinism hash is robust to
// the last float bit but still discriminates real position differences.
int32_t quant_mm(float metres) {
    return static_cast<int32_t>(std::lround(static_cast<double>(metres) * 1000.0));
}

void fnv1a(uint64_t& h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 0x100000001b3ull;
    }
}

// A lip candidate emitted by the scan before de-duplication.
struct LipCandidate {
    glm::vec3 crest{0.0f};
    glm::vec3 foot{0.0f};
    float drop = 0.0f;
    float run = 0.0f;
    float width = 0.0f;
    glm::vec2 flow{0.0f, 0.0f};
    float steepness = 0.0f;
    bool lake_outlet = false;
};

} // namespace

std::vector<WaterfallSite> DetectWaterfalls(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                            const WaterfallDetectParams& params) {
    std::vector<WaterfallSite> sites;

    const float step = std::max(0.5f, params.lattice_step);
    const int half = std::max(0, params.half_extent);
    const int side = (2 * half) / static_cast<int>(step) + 1;

    std::vector<LipCandidate> lips;

    // --- 1. Scan the river course for steep downstream drops. -------------
    // At each river cell, descend along the local height gradient over a short
    // run. If the channel surface drops >= min_drop with slope >= min_steepness
    // over that run, the cell is a waterfall lip. PURE: every value is a
    // function of (seed, params) via RiverInfluenceAt + GetTerrainHeightAt.
    for (int zi = 0; zi < side; ++zi) {
        const float cz = static_cast<float>(-half) + static_cast<float>(zi) * step;
        for (int xi = 0; xi < side; ++xi) {
            const float cx = static_cast<float>(-half) + static_cast<float>(xi) * step;

            const float influence = world.RiverInfluenceAt(cx, cz);
            if (influence <= params.river_threshold) {
                continue;
            }

            const float h0 = world.GetTerrainHeightAt(cx, cz);

            // Downhill gradient (central difference over one lattice step). The
            // flow heads toward the lower neighbour; a near-flat column has no
            // fall.
            const float hxp = world.GetTerrainHeightAt(cx + step, cz);
            const float hxn = world.GetTerrainHeightAt(cx - step, cz);
            const float hzp = world.GetTerrainHeightAt(cx, cz + step);
            const float hzn = world.GetTerrainHeightAt(cx, cz - step);
            glm::vec2 grad((hxp - hxn), (hzp - hzn)); // points uphill scaled by 2*step
            const float grad_len = std::sqrt(grad.x * grad.x + grad.y * grad.y);
            if (grad_len < 1e-4f) {
                continue;
            }
            // Downhill unit direction.
            const glm::vec2 down(-grad.x / grad_len, -grad.y / grad_len);

            // Walk downstream until the cumulative drop qualifies, the river
            // course ends, or we exceed max_run.
            const int max_steps = std::max(1, static_cast<int>(params.max_run / step));
            float run = 0.0f;
            float prev_h = h0;
            float best_drop = 0.0f;
            float best_run = 0.0f;
            glm::vec3 best_foot(cx, h0, cz);
            for (int s = 1; s <= max_steps; ++s) {
                const float sx = cx + down.x * step * static_cast<float>(s);
                const float sz = cz + down.y * step * static_cast<float>(s);
                const float hs = world.GetTerrainHeightAt(sx, sz);
                // Must keep descending; an uphill sample ends the run.
                if (hs > prev_h + 0.05f) {
                    break;
                }
                run += step;
                const float drop = h0 - hs;
                if (drop > best_drop) {
                    best_drop = drop;
                    best_run = run;
                    best_foot = glm::vec3(sx, hs, sz);
                }
                prev_h = hs;
                // Stop scanning once we have a clearly qualifying drop so the
                // run measures the cliff, not the whole valley descent.
                if (best_drop >= params.min_drop && best_run > 0.0f) {
                    const float slope_now = best_drop / std::max(best_run, step);
                    if (slope_now >= params.min_steepness) {
                        break;
                    }
                }
            }

            if (best_drop < params.min_drop || best_run <= 0.0f) {
                continue;
            }
            const float steepness = best_drop / std::max(best_run, step);
            if (steepness < params.min_steepness) {
                continue;
            }

            LipCandidate lip;
            lip.crest = glm::vec3(cx, h0, cz);
            lip.foot = best_foot;
            lip.drop = best_drop;
            lip.run = best_run;
            lip.steepness = steepness;
            lip.flow = down;
            // Channel width estimate: the influence band scaled by the carve
            // footprint (a centre column influence ~1 is the widest). Clamp to a
            // sane visual range so the sheet stays a channel, not a wall.
            lip.width = std::clamp(2.0f + influence * 6.0f, 2.0f, 12.0f);
            lips.push_back(lip);
        }
    }

    // --- 1b. Scan perched-lake / tarn RIMS for spill (outlet) dropoffs. ---
    //   / OWNER directive: waterfalls must also be CREATED BY a lake
    // spilling over its rim. A cell that is INSIDE a perched lake (WaterLevelAt >
    // SEA_LEVEL) whose downhill direction leaves the lake and FALLS away below the
    // lake surface over a short run is a spill point — a waterfall crest at the
    // rim. PURE: every value is a function of (seed, params) via WaterLevelAt +
    // GetTerrainHeightAt.
    {
        const float lake_eps = std::max(0.0f, params.lake_surface_epsilon);
        const int lake_max_steps = std::max(1, static_cast<int>(params.lake_outlet_max_run / step));
        for (int zi = 0; zi < side; ++zi) {
            const float cz = static_cast<float>(-half) + static_cast<float>(zi) * step;
            for (int xi = 0; xi < side; ++xi) {
                const float cx = static_cast<float>(-half) + static_cast<float>(xi) * step;

                // Must be inside (or at the surface of) a perched lake.
                const float lake_surface = world.WaterLevelAt(cx, cz);
                if (lake_surface <= Luminumbra::SEA_LEVEL + lake_eps) {
                    continue;
                }

                // Downhill terrain gradient (central difference). Points uphill.
                const float hxp = world.GetTerrainHeightAt(cx + step, cz);
                const float hxn = world.GetTerrainHeightAt(cx - step, cz);
                const float hzp = world.GetTerrainHeightAt(cx, cz + step);
                const float hzn = world.GetTerrainHeightAt(cx, cz - step);
                glm::vec2 grad((hxp - hxn), (hzp - hzn));
                const float grad_len = std::sqrt(grad.x * grad.x + grad.y * grad.y);
                if (grad_len < 1e-4f) {
                    continue; // flat lake interior — no spill direction
                }
                const glm::vec2 down(-grad.x / grad_len, -grad.y / grad_len);

                // The immediate downhill neighbour must leave the lake (be OUTSIDE
                // this lake basin), otherwise we are still in the lake interior.
                const float nx = cx + down.x * step;
                const float nz = cz + down.y * step;
                const float neigh_surface = world.WaterLevelAt(nx, nz);
                const bool neigh_outside =
                    neigh_surface <= Luminumbra::SEA_LEVEL + lake_eps ||
                    neigh_surface < lake_surface - 0.05f; // a lower water body downstream
                if (!neigh_outside) {
                    continue;
                }

                // Walk downhill past the rim; measure how far the terrain falls
                // below the lake surface (the spill drop). Require a steep dropoff.
                float run = 0.0f;
                float prev_h = lake_surface;
                float best_drop = 0.0f;
                float best_run = 0.0f;
                glm::vec3 best_foot(cx, lake_surface, cz);
                for (int s = 1; s <= lake_max_steps; ++s) {
                    const float sx = cx + down.x * step * static_cast<float>(s);
                    const float sz = cz + down.y * step * static_cast<float>(s);
                    const float hs = world.GetTerrainHeightAt(sx, sz);
                    if (hs > prev_h + 0.05f) {
                        break; // climbing again — past the dropoff
                    }
                    run += step;
                    const float drop = lake_surface - hs;
                    if (drop > best_drop) {
                        best_drop = drop;
                        best_run = run;
                        best_foot = glm::vec3(sx, hs, sz);
                    }
                    prev_h = hs;
                    if (best_drop >= params.lake_outlet_min_drop && best_run > 0.0f) {
                        const float slope_now = best_drop / std::max(best_run, step);
                        if (slope_now >= params.lake_outlet_min_steepness) {
                            break;
                        }
                    }
                }

                if (best_drop < params.lake_outlet_min_drop || best_run <= 0.0f) {
                    continue;
                }
                const float steepness = best_drop / std::max(best_run, step);
                if (steepness < params.lake_outlet_min_steepness) {
                    continue;
                }

                LipCandidate lip;
                // Crest sits at the lake surface at the rim (connected upstream).
                lip.crest = glm::vec3(cx, lake_surface, cz);
                lip.foot = best_foot;
                lip.drop = best_drop;
                lip.run = best_run;
                lip.steepness = steepness;
                lip.flow = down;
                lip.width = 4.0f; // a lake outlet reads as a modest channel
                lip.lake_outlet = true;
                lips.push_back(lip);
            }
        }
    }

    if (lips.empty()) {
        return sites;
    }

    // --- 1c. CONNECT each lip to the live water surfaces. -----
    // Upstream: pin the crest Y to the UPSTREAM water surface (river/lake) at the
    // lip so the sheet starts AT the water, not the bare channel floor. For lake
    // outlets the crest Y is already the lake surface; for river lips this lifts
    // the crest from the carved channel floor to the river/sea surface.
    // Downstream: raise the foot Y to max(terrain, WaterLevelAt(foot)) so when the
    // fall lands in an existing river/lake/sea the sheet reaches that surface.
    // NOTE: where the foot is dry we leave it at terrain — auto-creating a plunge
    // POOL (new standing water) would be a sim/world_hash-affecting change and is
    // DEFERRED (out of scope; do NOT modify WaterSystem). Render-only here.
    for (LipCandidate& lip : lips) {
        const float crest_water = world.WaterLevelAt(lip.crest.x, lip.crest.z);
        lip.crest.y = std::max(lip.crest.y, crest_water);
        const float foot_water = world.WaterLevelAt(lip.foot.x, lip.foot.z);
        lip.foot.y = std::max(lip.foot.y, foot_water);
        // Recompute drop after the surface connection (clamp non-negative).
        lip.drop = std::max(0.0f, lip.crest.y - lip.foot.y);
    }

    // --- 2. De-duplicate lips into discrete sites. -----------------------
    // Greedy clustering by crest proximity: process lips in a DETERMINISTIC
    // order (steepest drop first, ties broken by quantized position), and emit a
    // site only when its crest is farther than cluster_radius from every
    // already-emitted site crest. The steepest lip in a cluster wins (it is the
    // true cliff face). This is order-stable: the sort key is a pure function of
    // the lip fields.
    std::sort(lips.begin(), lips.end(), [](const LipCandidate& a, const LipCandidate& b) {
        if (a.drop != b.drop)
            return a.drop > b.drop;
        const int32_t ax = quant_mm(a.crest.x), bx = quant_mm(b.crest.x);
        if (ax != bx)
            return ax < bx;
        const int32_t az = quant_mm(a.crest.z), bz = quant_mm(b.crest.z);
        if (az != bz)
            return az < bz;
        return quant_mm(a.crest.y) < quant_mm(b.crest.y);
    });

    const float cluster_r2 = params.cluster_radius * params.cluster_radius;
    for (const LipCandidate& lip : lips) {
        // Surface connection can flatten a lip (foot water meets crest water) — a
        // sheet with no real fall would render as a degenerate quad; drop it.
        if (lip.drop < params.min_drop) {
            continue;
        }
        bool merged = false;
        for (const WaterfallSite& existing : sites) {
            const float dx = existing.crest.x - lip.crest.x;
            const float dz = existing.crest.z - lip.crest.z;
            if (dx * dx + dz * dz <= cluster_r2) {
                merged = true;
                break;
            }
        }
        if (merged) {
            continue;
        }
        WaterfallSite site;
        site.crest = lip.crest;
        site.foot = lip.foot;
        site.drop_height = lip.drop;
        site.run_length = lip.run;
        site.width = lip.width;
        site.flow_dir = lip.flow;
        site.steepness = lip.steepness;
        site.lake_outlet = lip.lake_outlet;
        sites.push_back(site);
    }

    // --- 3. Final stable spatial ordering. -------------------------------
    // Sort the emitted sites by quantized (x, z, y) so the returned vector is
    // byte-identical across runs regardless of the scan/cluster traversal.
    std::sort(sites.begin(), sites.end(), [](const WaterfallSite& a, const WaterfallSite& b) {
        const int32_t ax = quant_mm(a.crest.x), bx = quant_mm(b.crest.x);
        if (ax != bx)
            return ax < bx;
        const int32_t az = quant_mm(a.crest.z), bz = quant_mm(b.crest.z);
        if (az != bz)
            return az < bz;
        return quant_mm(a.crest.y) < quant_mm(b.crest.y);
    });

    return sites;
}

uint64_t HashWaterfallSites(const std::vector<WaterfallSite>& sites) {
    uint64_t h = 0xcbf29ce484222325ull; // FNV-1a offset basis
    const uint64_t count = sites.size();
    fnv1a(h, &count, sizeof(count));
    for (const WaterfallSite& s : sites) {
        const int32_t q[] = {
            quant_mm(s.crest.x),
            quant_mm(s.crest.y),
            quant_mm(s.crest.z),
            quant_mm(s.foot.x),
            quant_mm(s.foot.y),
            quant_mm(s.foot.z),
            quant_mm(s.drop_height),
            quant_mm(s.run_length),
            quant_mm(s.width),
            quant_mm(s.flow_dir.x),
            quant_mm(s.flow_dir.y),
            quant_mm(s.steepness),
            s.lake_outlet ? 1 : 0,
        };
        fnv1a(h, q, sizeof(q));
    }
    return h;
}

WaterfallDetectKey MakeWaterfallDetectKey(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                                          const WaterfallDetectParams& params) {
    WaterfallDetectKey key;
    key.seed = world.get_seed();
    key.half_extent = params.half_extent;
    key.lattice_step_milli = quant_mm(params.lattice_step);
    key.min_drop_milli = quant_mm(params.min_drop);
    key.min_steepness_milli = quant_mm(params.min_steepness);
    key.water_epoch = world.water_epoch(); // terraform edits re-key
    return key;
}

std::size_t WaterfallSiteCache::KeyHash::operator()(const WaterfallDetectKey& k) const noexcept {
    uint64_t h = 0xcbf29ce484222325ull;
    fnv1a(h, &k.seed, sizeof(k.seed));
    fnv1a(h, &k.half_extent, sizeof(k.half_extent));
    fnv1a(h, &k.lattice_step_milli, sizeof(k.lattice_step_milli));
    fnv1a(h, &k.min_drop_milli, sizeof(k.min_drop_milli));
    fnv1a(h, &k.min_steepness_milli, sizeof(k.min_steepness_milli));
    fnv1a(h, &k.water_epoch, sizeof(k.water_epoch)); //
    return static_cast<std::size_t>(h);
}

const std::vector<WaterfallSite>&
WaterfallSiteCache::sites_for(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                              const WaterfallDetectParams& params) {
    const WaterfallDetectKey key = MakeWaterfallDetectKey(world, params);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return it->second;
    }
    // epoch changes create new entries; keep the cache BOUNDED so a
    // terraform-happy session doesn't accumulate stale surveys (each holds a
    // full site vector). Dropping all entries on overflow is fine — the next
    // query recomputes exactly one survey.
    if (m_cache.size() >= 8) {
        m_cache.clear();
    }
    auto [inserted, ok] = m_cache.emplace(key, DetectWaterfalls(world, params));
    (void)ok;
    return inserted->second;
}

// the live upstream water factor (see the header contract).
float LiveWaterFactorAt(const Luminumbra::Systems::SHIELD_WorldSystem& world,
                        const WaterfallSite& site,
                        float full_depth) {
    // "Unknown" (no streamed grid at the crest) must read NEUTRAL, not
    // extinguished — distant sites keep their authored sheets.
    if (!world.debug_water_grid_at(site.crest.x, site.crest.z)) {
        return 1.0f;
    }
    const float surface = world.live_water_surface_at(site.crest.x, site.crest.z);
    const float terrain = world.GetTerrainHeightAt(site.crest.x, site.crest.z);
    const float depth = surface - terrain;
    if (full_depth <= 1e-4f)
        return depth > 0.0f ? 1.0f : 0.0f;
    return std::clamp(depth / full_depth, 0.0f, 1.0f);
}

} // namespace Luminumbra::Rendering
