#pragma once

#include "TreeLod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace Luminumbra::Rendering {

// Bounds contain the exact float spheres used by the instance test. Double
// endpoints and a dot-product error allowance keep cell rejection conservative.
struct StaticPropBounds {
    std::array<double, 3> low{INFINITY, INFINITY, INFINITY};
    std::array<double, 3> high{-INFINITY, -INFINITY, -INFINITY};
    bool unbounded = false;
    double maxRadius = 0.0;

    void Include(const std::array<float, 3>& center, float radius) {
        maxRadius = std::max(maxRadius, std::abs(double{radius}));
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(center[axis]) || !std::isfinite(radius)) {
                unbounded = true;
                return;
            }
            low[axis] = std::min(low[axis], double{center[axis]} - std::abs(double{radius}));
            high[axis] = std::max(high[axis], double{center[axis]} + std::abs(double{radius}));
        }
    }
    void Include(const StaticPropBounds& other) {
        unbounded = unbounded || other.unbounded;
        maxRadius = std::max(maxRadius, other.maxRadius);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], other.low[axis]);
            high[axis] = std::max(high[axis], other.high[axis]);
        }
    }
    bool Outside(const std::array<std::array<float, 4>, 6>& planes) const {
        if (unbounded)
            return false;
        if (low[0] > high[0])
            return true; // no drawable sphere in this LOD
        for (const auto& plane : planes) {
            double maximum = plane[3];
            double magnitude = std::abs(maximum) + maxRadius;
            double normalSum = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                maximum += double{plane[axis]} * (plane[axis] >= 0 ? high[axis] : low[axis]);
                normalSum += std::abs(double{plane[axis]});
                magnitude += std::abs(double{plane[axis]}) *
                             std::max(std::abs(low[axis]), std::abs(high[axis]));
            }
            // An overflowing legacy float dot may become NaN and accept the
            // sphere. Such a plane cannot conservatively reject its cell.
            if (magnitude >= std::numeric_limits<float>::max())
                continue;
            // The instance test subtracts radius even for non-unit/zero planes.
            maximum += maxRadius * std::max(0.0, 1.0 - normalSum);
            // The legacy float dot/radius comparison rounds several operations.
            // Include subnormal error too; nonfinite arithmetic never rejects.
            const double error = 64.0 * std::numeric_limits<float>::epsilon() * magnitude +
                                 64.0 * std::numeric_limits<float>::min();
            if (std::isfinite(maximum) && maximum < -error)
                return true;
        }
        return false;
    }
};

class StaticPropSpatialIndex {
public:
    struct Item {
        std::array<float, 3> position{};
        std::array<StaticPropBounds, kTreeLodCount> lodBounds{};
    };
    struct Work {
        std::size_t cells = 0;
        std::size_t instances = 0; // candidates entering the original instance loop
    };

    void Build(const std::vector<Item>& items) {
        // A balanced binary tree has fewer than 2*N nodes. Check multiplication
        // before allocating; all scratch and output buffers are bounded by N.
        if (items.size() > m_nodes.max_size() / 2 || items.size() > m_order.max_size())
            throw std::length_error("Static prop spatial index too large");
        // Publish only a complete index; an allocation failure leaves the
        // previous bounds and candidates valid for their original population.
        StaticPropSpatialIndex replacement;
        replacement.m_order.resize(items.size());
        replacement.m_candidates.reserve(items.size());
        replacement.m_sortScratch.resize(items.size());
        std::iota(replacement.m_order.begin(), replacement.m_order.end(), std::size_t{0});
        if (!items.empty())
            replacement.BuildNode(items, 0, items.size());
        m_nodes.swap(replacement.m_nodes);
        m_order.swap(replacement.m_order);
        m_candidates.swap(replacement.m_candidates);
        m_sortScratch.swap(replacement.m_sortScratch);
        m_work = {};
    }

    const std::vector<std::size_t>& Query(const std::array<float, 3>& camera,
                                          const std::array<std::array<float, 4>, 6>& planes,
                                          bool impostors) {
        m_work = {};
        m_candidates.clear();
        if (!m_nodes.empty())
            QueryNode(0, camera, planes, impostors);
        SortCandidates();
        m_work.instances = m_candidates.size();
        return m_candidates;
    }
    // Clock-free diagnostics; neither counters nor scratch affect selection.
    const Work& work() const {
        return m_work;
    }

private:
    static constexpr std::size_t kLeafSize = 32;
    struct Node {
        StaticPropBounds positions;
        std::array<StaticPropBounds, kTreeLodCount> lodBounds{};
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t left = 0;
        std::size_t right = 0;
    };
    std::vector<Node> m_nodes;
    std::vector<std::size_t> m_order, m_candidates, m_sortScratch;
    Work m_work;

    std::size_t BuildNode(const std::vector<Item>& items, std::size_t begin, std::size_t end) {
        const std::size_t index = m_nodes.size();
        m_nodes.push_back({});
        // Never keep a node reference across recursive vector growth.
        m_nodes[index].begin = begin;
        m_nodes[index].end = end;
        for (std::size_t offset = begin; offset < end; ++offset) {
            const Item& item = items[m_order[offset]];
            m_nodes[index].positions.Include(item.position, 0.0f);
            for (std::size_t lod = 0; lod < kTreeLodCount; ++lod)
                m_nodes[index].lodBounds[lod].Include(item.lodBounds[lod]);
        }
        if (end - begin > kLeafSize) {
            std::size_t axis = 0;
            for (std::size_t next = 1; next < 3; ++next) {
                if (m_nodes[index].positions.high[next] - m_nodes[index].positions.low[next] >
                    m_nodes[index].positions.high[axis] - m_nodes[index].positions.low[axis])
                    axis = next;
            }
            const std::size_t middle = begin + (end - begin) / 2;
            std::nth_element(m_order.begin() + static_cast<std::ptrdiff_t>(begin),
                             m_order.begin() + static_cast<std::ptrdiff_t>(middle),
                             m_order.begin() + static_cast<std::ptrdiff_t>(end),
                             [&](std::size_t a, std::size_t b) {
                                 const float pa = items[a].position[axis];
                                 const float pb = items[b].position[axis];
                                 // Finite coordinates sort first; ties retain a total order.
                                 if (std::isfinite(pa) != std::isfinite(pb))
                                     return std::isfinite(pa);
                                 if (std::isfinite(pa) && pa != pb)
                                     return pa < pb;
                                 return a < b;
                             });
            const std::size_t left = BuildNode(items, begin, middle);
            const std::size_t right = BuildNode(items, middle, end);
            m_nodes[index].left = left;
            m_nodes[index].right = right;
        }
        return index;
    }

    static unsigned
    LodMask(const StaticPropBounds& positions, const std::array<float, 3>& camera, bool impostors) {
        if (positions.unbounded)
            return 15u;
        double nearSquared = 0, farSquared = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(camera[axis]))
                return 15u;
            const double below = positions.low[axis] - camera[axis];
            const double above = double{camera[axis]} - positions.high[axis];
            const double nearest = std::max({0.0, below, above});
            const double farthest = std::max(std::abs(below), std::abs(above));
            nearSquared += nearest * nearest;
            farSquared += farthest * farthest;
        }
        // Cover subtraction, squared-length and sqrt rounding in glm::length.
        const double nearest = std::max(0.0, std::sqrt(nearSquared) * (1.0 - 0.00001));
        const double farthest = std::sqrt(farSquared) * (1.0 + 0.00001);
        unsigned mask = 0;
        for (const auto& config : {TreeLodConfig{}, AuthoredTreeLodConfig(impostors)}) {
            const std::array<double, 5> edges{
                0.0, config.lod1Distance, config.lod2Distance, config.lod3Distance, INFINITY};
            for (unsigned lod = 0; lod < kTreeLodCount; ++lod) {
                if (nearest <= edges[lod + 1] && farthest >= edges[lod])
                    mask |= 1u << lod;
            }
        }
        // Non-authored tree parts clamp LOD3 to LOD2.
        if ((mask & 8u) != 0)
            mask |= 4u;
        return mask;
    }
    void QueryNode(std::size_t index,
                   const std::array<float, 3>& camera,
                   const std::array<std::array<float, 4>, 6>& planes,
                   bool impostors) {
        ++m_work.cells;
        const Node& node = m_nodes[index];
        const unsigned mask = LodMask(node.positions, camera, impostors);
        bool outside = true;
        for (unsigned lod = 0; lod < kTreeLodCount; ++lod) {
            if ((mask & (1u << lod)) != 0 && !node.lodBounds[lod].Outside(planes)) {
                outside = false;
                break;
            }
        }
        if (outside)
            return;
        if (node.end - node.begin <= kLeafSize) {
            m_candidates.insert(m_candidates.end(),
                                m_order.begin() + static_cast<std::ptrdiff_t>(node.begin),
                                m_order.begin() + static_cast<std::ptrdiff_t>(node.end));
        } else {
            QueryNode(node.left, camera, planes, impostors);
            QueryNode(node.right, camera, planes, impostors);
        }
    }
    void SortCandidates() {
        // Fixed-pass radix sort restores original registry order in O(candidates),
        // without clearing or scanning a population-sized visibility bitmap.
        for (std::size_t byte = 0; m_candidates.size() > 1 && byte < sizeof(std::size_t); ++byte) {
            std::array<std::size_t, 256> offsets{};
            const std::size_t shift = byte * 8;
            for (const auto index : m_candidates)
                ++offsets[(index >> shift) & 255u];
            std::size_t sum = 0;
            for (auto& offset : offsets) {
                const std::size_t count = offset;
                offset = sum;
                sum += count;
            }
            for (const auto index : m_candidates)
                m_sortScratch[offsets[(index >> shift) & 255u]++] = index;
            std::copy_n(m_sortScratch.begin(), m_candidates.size(), m_candidates.begin());
        }
    }
};

// Retain immutable instance bytes while the ordered instance IDs are unchanged.
// Begin/Select/Finish also handle removal, insertion and empty batches without a
// population-sized comparison or a second matrix/tint stream.
class StaticPropStreamSelection {
public:
    void Begin() {
        // An interrupted collection may have selected an ID before its matrix
        // or tint allocation failed. Rebuild that stream on the next frame.
        if (!m_finished)
            m_ids.clear();
        m_cursor = 0;
        m_changed = false;
        m_finished = false;
    }
    bool Select(std::size_t index) {
        if (!m_changed && m_cursor < m_ids.size() && m_ids[m_cursor] == index) {
            ++m_cursor;
            return false;
        }
        m_changed = true;
        m_ids.resize(m_cursor);
        m_ids.push_back(index);
        ++m_cursor;
        return true;
    }
    bool Finish() {
        m_changed = m_changed || m_cursor != m_ids.size();
        m_ids.resize(m_cursor);
        m_finished = true;
        return m_changed;
    }
    std::size_t size() const {
        return m_cursor;
    }

private:
    std::vector<std::size_t> m_ids;
    std::size_t m_cursor = 0;
    bool m_changed = false;
    bool m_finished = true;
};

} // namespace Luminumbra::Rendering
