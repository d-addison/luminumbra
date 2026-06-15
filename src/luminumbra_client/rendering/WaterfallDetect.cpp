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
};

} // namespace

std::vector<WaterfallSite> DetectWaterfalls(
    const Luminumbra::Systems::SHIELD_WorldSystem& world,
    const WaterfallDetectParams& params) {
    std::vector<WaterfallSite> sites;

    // Rivers carve the drops; with no river course there are no waterfalls.
    if (!world.get_params().rivers_enabled) {
        return sites;
    }

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
            const int max_steps =
                std::max(1, static_cast<int>(params.max_run / step));
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

    if (lips.empty()) {
        return sites;
    }

    // --- 2. De-duplicate lips into discrete sites. -----------------------
    // Greedy clustering by crest proximity: process lips in a DETERMINISTIC
    // order (steepest drop first, ties broken by quantized position), and emit a
    // site only when its crest is farther than cluster_radius from every
    // already-emitted site crest. The steepest lip in a cluster wins (it is the
    // true cliff face). This is order-stable: the sort key is a pure function of
    // the lip fields.
    std::sort(lips.begin(), lips.end(), [](const LipCandidate& a, const LipCandidate& b) {
        if (a.drop != b.drop) return a.drop > b.drop;
        const int32_t ax = quant_mm(a.crest.x), bx = quant_mm(b.crest.x);
        if (ax != bx) return ax < bx;
        const int32_t az = quant_mm(a.crest.z), bz = quant_mm(b.crest.z);
        if (az != bz) return az < bz;
        return quant_mm(a.crest.y) < quant_mm(b.crest.y);
    });

    const float cluster_r2 = params.cluster_radius * params.cluster_radius;
    for (const LipCandidate& lip : lips) {
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
        sites.push_back(site);
    }

    // --- 3. Final stable spatial ordering. -------------------------------
    // Sort the emitted sites by quantized (x, z, y) so the returned vector is
    // byte-identical across runs regardless of the scan/cluster traversal.
    std::sort(sites.begin(), sites.end(), [](const WaterfallSite& a, const WaterfallSite& b) {
        const int32_t ax = quant_mm(a.crest.x), bx = quant_mm(b.crest.x);
        if (ax != bx) return ax < bx;
        const int32_t az = quant_mm(a.crest.z), bz = quant_mm(b.crest.z);
        if (az != bz) return az < bz;
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
            quant_mm(s.crest.x), quant_mm(s.crest.y), quant_mm(s.crest.z),
            quant_mm(s.foot.x),  quant_mm(s.foot.y),  quant_mm(s.foot.z),
            quant_mm(s.drop_height), quant_mm(s.run_length), quant_mm(s.width),
            quant_mm(s.flow_dir.x),  quant_mm(s.flow_dir.y), quant_mm(s.steepness),
        };
        fnv1a(h, q, sizeof(q));
    }
    return h;
}

WaterfallDetectKey MakeWaterfallDetectKey(
    const Luminumbra::Systems::SHIELD_WorldSystem& world,
    const WaterfallDetectParams& params) {
    WaterfallDetectKey key;
    key.seed = world.get_seed();
    key.half_extent = params.half_extent;
    key.lattice_step_milli = quant_mm(params.lattice_step);
    key.min_drop_milli = quant_mm(params.min_drop);
    key.min_steepness_milli = quant_mm(params.min_steepness);
    return key;
}

std::size_t WaterfallSiteCache::KeyHash::operator()(const WaterfallDetectKey& k) const noexcept {
    uint64_t h = 0xcbf29ce484222325ull;
    fnv1a(h, &k.seed, sizeof(k.seed));
    fnv1a(h, &k.half_extent, sizeof(k.half_extent));
    fnv1a(h, &k.lattice_step_milli, sizeof(k.lattice_step_milli));
    fnv1a(h, &k.min_drop_milli, sizeof(k.min_drop_milli));
    fnv1a(h, &k.min_steepness_milli, sizeof(k.min_steepness_milli));
    return static_cast<std::size_t>(h);
}

const std::vector<WaterfallSite>& WaterfallSiteCache::sites_for(
    const Luminumbra::Systems::SHIELD_WorldSystem& world,
    const WaterfallDetectParams& params) {
    const WaterfallDetectKey key = MakeWaterfallDetectKey(world, params);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return it->second;
    }
    auto [inserted, ok] = m_cache.emplace(key, DetectWaterfalls(world, params));
    (void)ok;
    return inserted->second;
}

} // namespace Luminumbra::Rendering
