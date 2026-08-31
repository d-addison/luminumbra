#pragma once

// deterministic grid A* pathfinding — the navigation foundation for
// obstacle-aware locomotion (the roadmap's "pathfinding/obstacle avoidance").
//
// Self-contained and PURE: operates on a caller-supplied 2D walkability grid and
// returns a cell path. It does NOT touch the ECS, the world, or world_hash — a
// system that wires this into InstinctLocomotion (steering along the path) is a
// later, explicitly data-flagged step, exactly like flee/separation. Keeping the
// algorithm standalone makes it unit-testable without any world coupling.
//
// DETERMINISM CONTRACT (sim-safe): integer cell math only; the open-set ordering
// breaks f-score ties by a STABLE secondary key (cell index), so the expansion
// order — and therefore the returned path — never depends on container internals
// or insertion order. No RNG, no wall-clock, no libm transcendentals. g/f costs
// are fixed-point (scaled ints) so there is no float-compare nondeterminism:
// orthogonal step = 10, diagonal step = 14 (the standard 10/14 octile lattice),
// heuristic = octile distance in the same units (admissible -> shortest path).

#include <cstdint>
#include <vector>

namespace luminumbra::ai {

struct GridCoord {
    int x = 0;
    int z = 0;
    bool operator==(const GridCoord& o) const {
        return x == o.x && z == o.z;
    }
};

// A rectangular walkability grid. `walkable[z*width + x]` is true when an agent
// may occupy cell (x,z). Out-of-bounds is treated as blocked.
class NavGrid {
public:
    NavGrid(int width, int height, std::vector<std::uint8_t> walkable)
        : m_w(width > 0 ? width : 0)
        , m_h(height > 0 ? height : 0)
        , m_walkable(std::move(walkable)) {}

    [[nodiscard]] int width() const {
        return m_w;
    }
    [[nodiscard]] int height() const {
        return m_h;
    }

    [[nodiscard]] bool in_bounds(int x, int z) const {
        return x >= 0 && z >= 0 && x < m_w && z < m_h;
    }
    [[nodiscard]] bool walkable(int x, int z) const {
        if (!in_bounds(x, z))
            return false;
        const std::size_t i = static_cast<std::size_t>(z) * static_cast<std::size_t>(m_w) +
                              static_cast<std::size_t>(x);
        return i < m_walkable.size() && m_walkable[i] != 0u;
    }

private:
    int m_w;
    int m_h;
    std::vector<std::uint8_t> m_walkable;
};

namespace detail {

// Octile heuristic in 10/14 units: 14*min(dx,dz) + 10*|dx-dz|. Admissible and
// consistent for the 8-neighbour 10/14 lattice -> A* returns a true shortest path.
inline int octile(int ax, int az, int bx, int bz) {
    int dx = ax > bx ? ax - bx : bx - ax;
    int dz = az > bz ? az - bz : bz - az;
    int lo = dx < dz ? dx : dz;
    int hi = dx < dz ? dz : dx;
    return 14 * lo + 10 * (hi - lo);
}

} // namespace detail

// Find a shortest 8-connected path from start to goal over `grid`. Returns the
// full cell sequence INCLUDING start and goal, or an empty vector when start/goal
// are blocked/out-of-bounds or no path exists. Diagonal moves are disallowed when
// they would "cut" a blocked orthogonal corner (no clipping through wall corners).
//
// Deterministic: ties in the priority frontier resolve by ascending cell index,
// so the path is a pure function of (grid, start, goal).
inline std::vector<GridCoord> FindPath(const NavGrid& grid, GridCoord start, GridCoord goal) {
    const int W = grid.width();
    const int H = grid.height();
    if (W <= 0 || H <= 0)
        return {};
    if (!grid.walkable(start.x, start.z) || !grid.walkable(goal.x, goal.z))
        return {};
    if (start == goal)
        return {start};

    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    const int INF = 0x3fffffff;
    auto idx = [W](int x, int z) {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(W) +
               static_cast<std::size_t>(x);
    };

    std::vector<int> g(N, INF);            // best known cost from start
    std::vector<std::int64_t> came(N, -1); // predecessor cell index (-1 = none)
    std::vector<std::uint8_t> closed(N, 0u);

    // Open frontier as a binary min-heap of (f, g, index). Tie-break: lower f, then
    // lower g (prefer progress), then lower index (STABLE -> deterministic). Hand-
    // rolled so the comparator is explicit and container-independent.
    struct Node {
        int f;
        int gc;
        std::size_t i;
    };
    auto worse = [](const Node& a, const Node& b) {
        if (a.f != b.f)
            return a.f > b.f;
        if (a.gc != b.gc)
            return a.gc > b.gc;
        return a.i > b.i;
    };
    std::vector<Node> heap;
    auto push = [&](Node n) {
        heap.push_back(n);
        std::size_t c = heap.size() - 1;
        while (c > 0) {
            std::size_t p = (c - 1) / 2;
            if (worse(heap[p], heap[c])) {
                std::swap(heap[p], heap[c]);
                c = p;
            } else
                break;
        }
    };
    auto pop = [&]() {
        Node top = heap.front();
        heap.front() = heap.back();
        heap.pop_back();
        std::size_t c = 0;
        const std::size_t n = heap.size();
        while (true) {
            std::size_t l = 2 * c + 1, r = 2 * c + 2, best = c;
            if (l < n && worse(heap[best], heap[l]))
                best = l;
            if (r < n && worse(heap[best], heap[r]))
                best = r;
            if (best == c)
                break;
            std::swap(heap[best], heap[c]);
            c = best;
        }
        return top;
    };

    g[idx(start.x, start.z)] = 0;
    push({detail::octile(start.x, start.z, goal.x, goal.z), 0, idx(start.x, start.z)});

    const std::size_t goal_i = idx(goal.x, goal.z);
    // 8 neighbours: 4 orthogonal (cost 10) then 4 diagonal (cost 14). Fixed order
    // for deterministic expansion (the heap tie-break also pins it).
    const int ox[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int oz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const int oc[8] = {10, 10, 10, 10, 14, 14, 14, 14};

    while (!heap.empty()) {
        Node cur = pop();
        if (closed[cur.i] != 0u)
            continue; // stale heap entry
        closed[cur.i] = 1u;
        if (cur.i == goal_i)
            break;

        const int cx = static_cast<int>(cur.i % static_cast<std::size_t>(W));
        const int cz = static_cast<int>(cur.i / static_cast<std::size_t>(W));
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + ox[k];
            const int nz = cz + oz[k];
            if (!grid.walkable(nx, nz))
                continue;
            if (k >= 4) {
                // Disallow diagonal corner-cutting: both shared orthogonal cells
                // must be walkable, else the diagonal would clip a wall corner.
                if (!grid.walkable(cx + ox[k], cz) || !grid.walkable(cx, cz + oz[k]))
                    continue;
            }
            const std::size_t ni = idx(nx, nz);
            if (closed[ni] != 0u)
                continue;
            const int ng = g[cur.i] + oc[k];
            if (ng < g[ni]) {
                g[ni] = ng;
                came[ni] = static_cast<std::int64_t>(cur.i);
                push({ng + detail::octile(nx, nz, goal.x, goal.z), ng, ni});
            }
        }
    }

    if (came[goal_i] < 0 && goal_i != idx(start.x, start.z))
        return {}; // unreachable

    // Reconstruct start..goal.
    std::vector<GridCoord> path;
    for (std::int64_t i = static_cast<std::int64_t>(goal_i); i >= 0;
         i = came[static_cast<std::size_t>(i)]) {
        const int x = static_cast<int>(static_cast<std::size_t>(i) % static_cast<std::size_t>(W));
        const int z = static_cast<int>(static_cast<std::size_t>(i) / static_cast<std::size_t>(W));
        path.push_back({x, z});
        if (static_cast<std::size_t>(i) == idx(start.x, start.z))
            break;
    }
    std::vector<GridCoord> out(path.rbegin(), path.rend());
    if (out.empty() || !(out.front() == start))
        return {}; // safety: no real path
    return out;
}

} // namespace luminumbra::ai
