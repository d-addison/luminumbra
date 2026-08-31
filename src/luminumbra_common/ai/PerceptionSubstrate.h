#pragma once

// Shared AI perception substrate used by both AI stacks:
//   * the GOAP InstinctSystem (replicated server NPCs) — a per-agent scan over
//     the tick's OpportunityComponent set, distance-gated by each opportunity's
//     influence radius (InstinctSystem.cpp);
//   * the IAUS CreatureBrain (ambient wildlife) — a per-creature genome-gated
//     vision-cone/hearing scan over the pre-tick snapshot (CreatureBrainSystem.h).
// This file factors their shared primitive into a deterministic,
// spatially-bucketed neighbour/stimulus scan producing a reusable
// PerceptionSnapshot. Both systems use it by default and retain their inline
// scans only as tested equivalence references.
//
// DETERMINISM (sim contract): perceivers/sources are visited in the caller's
// canonical ordinal order (the caller pre-sorts by its stable key — opportunity
// id string, entt id,...), so the snapshot order never depends on registry
// storage. The distance metric matches the WIRED consumer exactly: the GOAP
// gather rounds the 3-D Euclidean distance in DOUBLE via std::sqrt (IEEE-754
// correctly-rounded -> cross-platform stable, unlike libm transcendentals) and
// std::round, so this substrate uses the identical formula. A libm-free float
// variant (DeterministicMath::Sqrt) is used by CreatureBrain; see the
// per-source-vs-per-perceiver radius note on PerceptionField below.
//
// SHARED-PRIMITIVE / CreatureBrain adoption note: sources carry a per-SOURCE
// influence radius (the opportunity model). The CreatureBrain model is a per-
// PERCEIVER sense radius with cone/hearing refinement. CreatureBrain buckets
// sources at cell_size == the perceiver's max sense radius, radius-queries the
// 3x3 block, and applies its own cone/hearing gate on the returned snapshot. The
// bucketing + deterministic snapshot below is the reusable half; the sensory GATE
// stays consumer policy.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "SpatialGrid.h"

namespace luminumbra::ai {

// A perceivable source (opportunity / stimulus / creature) fed into the scan.
// `index` is the caller's canonical ordinal — the caller has ALREADY sorted its
// sources by its stable key and passes each source's position in that order, so
// the snapshot (sorted by `index`) reproduces the caller's inline id-ordered
// gather exactly. `has_position == false` => a GLOBAL source, always perceived at
// distance 0 (mirrors an opportunity with no TransformComponent). `radius` is the
// source's own influence radius; <= 0 means UNGATED (perceived at any distance,
// matching OpportunityComponent's "0 = unbounded"). `y` is carried so the metric
// is full 3-D like the GOAP gather; XZ-only consumers pass y = 0.
struct PerceptionSourceInput {
    std::uint32_t index = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    bool has_position = false;
};

// The perceiver. When `has_position == false` EVERY source is perceived at
// distance 0 (mirrors the inline "agent without a TransformComponent" branch,
// which skips the whole distance/radius computation).
struct PerceptionQueryInput {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool has_position = false;
};

// One perceived source in the snapshot: the caller ordinal plus the distance the
// inline gather would have recorded (Round4 of the 3-D Euclidean distance in
// DOUBLE — identical to InstinctSystem — or 0 for a global source / positionless
// perceiver). The distance is load-bearing: the GOAP planner scores it directly.
struct PerceivedSource {
    std::uint32_t index = 0;
    double distance = 0.0;
};

// The reusable per-perceiver perception snapshot: perceived sources in caller-
// ordinal order (== the inline id-ordered gather order, since ordinals are the
// pre-sorted positions).
struct PerceptionSnapshot {
    std::vector<PerceivedSource> perceived;
};

// Match InstinctSystem's Round4 (std::round(v * 1e4) / 1e4) so the recorded
// distance is byte-identical to the inline gather.
[[nodiscard]] inline double PerceptionRound4(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

// 3-D Euclidean distance in DOUBLE, then Round4 — the exact InstinctSystem
// formula. Float source/query coords promote to double before subtraction, which
// is bit-for-bit what `static_cast<double>(agent.x) - source.position[0]` does.
[[nodiscard]] inline double PerceptionRound4Distance(const PerceptionQueryInput& q,
                                                     const PerceptionSourceInput& s) {
    const double dx = static_cast<double>(q.x) - static_cast<double>(s.x);
    const double dy = static_cast<double>(q.y) - static_cast<double>(s.y);
    const double dz = static_cast<double>(q.z) - static_cast<double>(s.z);
    return PerceptionRound4(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// The shared substrate: Build the source set ONCE per tick, then Query it once
// per perceiver. Positioned, radius-gated sources are bucketed into a
// UniformSpatialGrid so a perceiver only distance-tests a 3x3 cell block instead
// of every source; positionless and ungated (radius <= 0) sources are always
// perceived. The bucketing is a PURE accelerator — it only narrows the candidate
// set before the EXACT inline distance/radius filter, then the result is sorted
// by caller ordinal, so the snapshot is byte-identical to the inline full scan.
class PerceptionField {
public:
    // Partition the sources and build the grid over the radius-gated ones.
    void Build(const std::vector<PerceptionSourceInput>& sources) {
        m_positionless.clear();
        m_ungated.clear();
        m_gated.clear();
        m_grid.reset();

        float max_radius = 0.0f;
        for (const PerceptionSourceInput& s : sources) {
            if (!s.has_position) {
                m_positionless.push_back(s.index); // global: always perceived, dist 0
            } else if (s.radius <= 0.0f) {
                m_ungated.push_back(s); // ungated: always perceived, dist computed
            } else {
                m_gated.push_back(s); // radius-gated: bucketed
                if (s.radius > max_radius)
                    max_radius = s.radius;
            }
        }

        if (!m_gated.empty()) {
            // Cell size >= the largest gate radius (plus a 1 m margin that also
            // absorbs the Round4 tolerance, ~5e-5) GUARANTEES the fixed 3x3 query
            // is a superset of every true neighbour: a gated source with
            // Round4(dist) <= radius <= max_radius has XZ distance <= 3-D distance
            // <= max_radius < cell_size, so each axis differs by at most one cell.
            const float cell_size = max_radius + 1.0f;
            m_grid.emplace(cell_size);
            std::vector<GridPoint> pts;
            pts.reserve(m_gated.size());
            for (std::uint32_t gi = 0; gi < static_cast<std::uint32_t>(m_gated.size()); ++gi) {
                pts.push_back({gi, m_gated[gi].x, m_gated[gi].z}); // grid payload = m_gated index
            }
            m_grid->Build(pts);
        }
    }

    // Fill `out` with the perceiver's perceived-source snapshot (cleared first).
    void Query(const PerceptionQueryInput& q, PerceptionSnapshot& out) const {
        out.perceived.clear();

        // Positionless sources are perceived at distance 0 regardless of the
        // perceiver (inline: the radius block is entered only when BOTH sides
        // have a position).
        for (std::uint32_t idx : m_positionless) {
            out.perceived.push_back({idx, 0.0});
        }

        if (!q.has_position) {
            // Perceiver has no world position: EVERY remaining source is
            // perceived at distance 0 (inline skips the distance/radius block).
            for (const PerceptionSourceInput& s : m_ungated) {
                out.perceived.push_back({s.index, 0.0});
            }
            for (const PerceptionSourceInput& s : m_gated) {
                out.perceived.push_back({s.index, 0.0});
            }
            SortByIndex(out.perceived);
            return;
        }

        // Ungated positioned sources: always perceived, distance computed.
        for (const PerceptionSourceInput& s : m_ungated) {
            out.perceived.push_back({s.index, PerceptionRound4Distance(q, s)});
        }

        // Radius-gated sources: 3x3 bucket prune, then the EXACT inline filter
        // (Round4(dist) > radius -> skip). Distances are byte-identical, so the
        // included set matches the inline full scan.
        if (m_grid) {
            m_scratch.clear();
            m_grid->QueryRadius(q.x, q.z, m_scratch);
            for (std::uint32_t gi : m_scratch) {
                const PerceptionSourceInput& s = m_gated[gi];
                const double dist = PerceptionRound4Distance(q, s);
                if (dist > static_cast<double>(s.radius)) {
                    continue; // outside this source's influence radius
                }
                out.perceived.push_back({s.index, dist});
            }
        }

        SortByIndex(out.perceived);
    }

private:
    // Ordinals are unique, so ascending-index order reproduces the inline gather
    // order (which appends in ascending index order) exactly; stability is moot.
    static void SortByIndex(std::vector<PerceivedSource>& v) {
        std::sort(v.begin(), v.end(), [](const PerceivedSource& a, const PerceivedSource& b) {
            return a.index < b.index;
        });
    }

    std::vector<std::uint32_t> m_positionless;    // global sources (dist 0)
    std::vector<PerceptionSourceInput> m_ungated; // positioned, radius <= 0
    std::vector<PerceptionSourceInput> m_gated;   // positioned, radius > 0 (bucketed)
    std::optional<UniformSpatialGrid> m_grid;     // grid over m_gated (payload = m_gated index)
    mutable std::vector<std::uint32_t> m_scratch; // reused 3x3 query buffer
};

} // namespace luminumbra::ai
