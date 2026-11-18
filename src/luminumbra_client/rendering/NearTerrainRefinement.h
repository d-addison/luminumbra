#pragma once

#include "UploadBudget.h"
#include "luminumbra_common/world/NearTerrainMeshSnapshot.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Rendering {

inline constexpr std::size_t kNearTerrainMaxInputTriangles = 2048;
inline constexpr std::size_t kNearTerrainMaxInputVertices = 6144;
inline constexpr std::size_t kNearTerrainMaxOutputTriangles = 6 * kNearTerrainMaxInputTriangles;
inline constexpr std::size_t kNearTerrainMaxAddedVertices = 5 * kNearTerrainMaxInputTriangles / 2;
inline constexpr float kNearTerrainRadiusMeters = 32.0f;
inline constexpr float kNearTerrainMaxDisplacementMeters = 0.25f;

struct NearTerrainRefinementInput {
    std::span<const VoxelVertex> vertices;
    std::span<const u32> indices;
    // Complete chunk-local padded lattice, X fastest, then Y, then Z.
    std::span<const float> densities;
    int sample_step = 0;
    // Eye-to-chunk-centre distance squared in metres. Whole admitted chunks
    // fit within 32 + 8*sqrt(3) metres of the eye (less than 46 metres).
    float chunk_distance_squared = -1.0f;
    bool enabled = false;
    bool generated_current_params = false;
    // Caller must establish an unedited one-cell halo from the same immutable
    // snapshot as the mesh/lattice. Missing or stale evidence must remain false.
    bool unedited_halo = false;
};

struct NearTerrainRenderMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
};

enum class NearTerrainRefinementResult {
    Refined,
    Ineligible,
    InvalidMesh,
    CapacityExceeded,
};

// Build-time render copy only; never pass the output to collision or authority.
// All original vertices and every topological/chunk-face boundary edge survive
// exactly. Only shared manifold interior edges split, once each; triangle fans
// join those splits without T-junctions. Heights use cubic Hermite controls
// derived from normals, with vertical displacement clamped to 0.25 m. X/Z stay
// barycentric, so every resulting surface point is within that bound of the
// original triangle at the same X/Z (allow 0.001 m for float interpolation
// roundoff). Non-height-like lattices refuse wholesale. The mesh and lattice
// must come from the same immutable snapshot; this kernel never accesses a world.
// Child triangles preserve the source's flat last-vertex material convention.
//
// Work is capped before scanning: 4913 densities, 6144 vertices, 2048 triangles,
// and sorting 6144 edge records. At most 12288 output triangles and 5120 added
// vertices. Vector payload peak (excluding input/prior output and allocator
// overhead) is <= (6144+5120)*sizeof(VoxelVertex) + 12288*3*sizeof(u32)
// + 6144*(3*sizeof(u32)+sizeof(u32)). No per-frame work or retained scratch.
// Refusal and allocation exceptions leave output unchanged; success replaces it.
[[nodiscard]] NearTerrainRefinementResult RefineNearTerrain(const NearTerrainRefinementInput& input,
                                                            NearTerrainRenderMesh& output);

// A weak upload stamp distinguishes a refused/expired request from disabled output.
// It owns no chunk or density storage and remains valid across cache eviction.
struct NearTerrainUploadState {
    std::weak_ptr<const World::NearTerrainMeshSnapshot> evidence;
    bool requested = false;

    [[nodiscard]] bool
    matches(const std::shared_ptr<const World::NearTerrainMeshSnapshot>& current) const {
        return requested == static_cast<bool>(current) &&
               (!requested || evidence.lock() == current);
    }
    void set(const std::shared_ptr<const World::NearTerrainMeshSnapshot>& current) {
        evidence = current;
        requested = static_cast<bool>(current);
    }
};

enum class NearTerrainUploadResult {
    Unrefined,
    Refined,
    Deferred
};

// Owner/context-thread render cache, like the source chunk meshes and SDF. Fixed FIFO of 64
// entries: at most 64 * 452 KiB geometry plus 64 * 4913 floats of evidence; no chunk ownership.
// Lookups and retirement are expected O(1), eviction is O(1), and clearing visits 64 slots.
class NearTerrainRefinementCache {
public:
    static constexpr std::size_t kCapacity = 64;

    bool set_enabled(bool enabled) {
        if (m_enabled == enabled) {
            return false;
        }
        clear();
        m_enabled = enabled;
        return true;
    }
    [[nodiscard]] bool enabled() const {
        return m_enabled;
    }
    void clear() {
        m_index.clear();
        for (auto& entry : m_entries) {
            entry = {};
        }
        m_next = 0;
    }
    void forget(ChunkID id) {
        const auto found = m_index.find(id);
        if (found != m_index.end()) {
            m_entries[found->second] = {};
            m_index.erase(found);
        }
    }
    std::shared_ptr<const World::NearTerrainMeshSnapshot>
    observe(const Chunk& chunk,
            u32 version,
            const std::shared_ptr<const World::NearTerrainMeshSnapshot>& evidence,
            float distance_squared) {
        const auto current =
            m_enabled && evidence && evidence->eligible(chunk, version, distance_squared) ? evidence
                                                                                          : nullptr;
        const auto found = m_index.find(chunk.get_id());
        if (found != m_index.end()) {
            const auto& entry = m_entries[found->second];
            if (entry.version != version || entry.evidence != current) {
                forget(chunk.get_id());
            }
        }
        return current;
    }
    // Admission reserves both the original copy and the maximum refined upload.
    // Kernel limits precede arithmetic; larger/refused meshes retain their exact cost.
    static std::size_t upload_cost(bool requested,
                                   std::size_t vertices,
                                   std::size_t indices,
                                   std::size_t original_bytes) {
        if (!requested || vertices > kNearTerrainMaxInputVertices ||
            indices / 3 > kNearTerrainMaxInputTriangles || indices % 3 != 0) {
            return original_bytes;
        }
        std::size_t refined_bytes = 0;
        if (!mesh_upload_bytes(vertices + 5 * (indices / 3) / 2,
                               sizeof(VoxelVertex),
                               indices * 6,
                               refined_bytes) ||
            original_bytes > std::numeric_limits<std::size_t>::max() - refined_bytes) {
            return original_bytes;
        }
        return original_bytes + refined_bytes;
    }
    NearTerrainUploadResult
    prepare(const Chunk& chunk,
            u32 version,
            const std::shared_ptr<const World::NearTerrainMeshSnapshot>& evidence,
            float distance_squared,
            std::vector<VoxelVertex>& vertices,
            std::vector<u32>& indices,
            double elapsed_ms) {
        const auto current = observe(chunk, version, evidence, distance_squared);
        if (!current) {
            return NearTerrainUploadResult::Unrefined;
        }
        auto found = m_index.find(chunk.get_id());
        // Cached refusals need neither kernel work nor a replacement copy, even
        // when baseline upload preparation has exhausted the frame deadline.
        if (found != m_index.end() && !m_entries[found->second].refined) {
            return NearTerrainUploadResult::Unrefined;
        }
        if (!std::isfinite(elapsed_ms) || elapsed_ms < 0 ||
            elapsed_ms >= kTerrainUploadLimits.milliseconds) {
            return NearTerrainUploadResult::Deferred;
        }
        if (found == m_index.end()) {
            // Both world publishers retain classification only. The matching
            // version/halo proof above certifies this owner-thread SDF too.
            // Copy exactly 17^3 samples before the kernel; never move authority
            // data or allocate for a partial/unbounded lattice. The immutable
            // local copy lives through refinement and is released afterwards.
            const std::vector<float> density_copy =
                current->densities.empty() &&
                        chunk.sdf_data.size() == World::kNearTerrainDensityCount
                    ? chunk.sdf_data
                    : std::vector<float>{};
            Entry replacement;
            replacement.id = chunk.get_id();
            replacement.version = version;
            replacement.evidence = current;
            ++m_builds;
            replacement.refined =
                RefineNearTerrain({.vertices = vertices,
                                   .indices = indices,
                                   .densities = current->densities.empty()
                                                    ? std::span<const float>(density_copy)
                                                    : std::span<const float>(current->densities),
                                   .sample_step = current->sample_step,
                                   .chunk_distance_squared = distance_squared,
                                   .enabled = true,
                                   .generated_current_params = current->generated_current_params,
                                   .unedited_halo = current->unedited_halo},
                                  replacement.mesh) == NearTerrainRefinementResult::Refined;
            // Allocate the index node before eviction or the noexcept move.
            // At most 65 index nodes exist during this commit; failure preserves
            // all resident entries and the original payload.
            found = m_index.emplace(chunk.get_id(), m_next).first;
            if (m_entries[m_next].id) {
                forget(*m_entries[m_next].id);
            }
            m_entries[m_next] = std::move(replacement);
            m_next = (m_next + 1) % kCapacity;
        }
        const auto& entry = m_entries[found->second];
        if (!entry.refined) {
            return NearTerrainUploadResult::Unrefined;
        }
        // Build the complete replacement before changing either payload vector.
        auto replacement = entry.mesh;
        vertices.swap(replacement.vertices);
        indices.swap(replacement.indices);
        return NearTerrainUploadResult::Refined;
    }
    // Bounded cache/CPU-work diagnostics for upload telemetry and inspection.
    [[nodiscard]] std::size_t size() const {
        return m_index.size();
    }
    [[nodiscard]] std::size_t builds() const {
        return m_builds;
    }

private:
    struct Entry {
        std::optional<ChunkID> id;
        u32 version = 0;
        std::shared_ptr<const World::NearTerrainMeshSnapshot> evidence;
        NearTerrainRenderMesh mesh;
        bool refined = false;
    };
    std::array<Entry, kCapacity> m_entries{};
    std::unordered_map<ChunkID, std::size_t> m_index;
    std::size_t m_next = 0;
    std::size_t m_builds = 0;
    bool m_enabled = false;
};

// Both queues share one upload budget. Baseline uploads retain the guaranteed
// progress attempt; optional replacements run only in remaining bytes/time.
// Each queue has one entry per participating chunk, visited at most once here.
template<typename Item, typename Cost, typename Adopt, typename Elapsed>
UploadWorkStats drain_terrain_uploads(UploadQueue<Item>& baseline,
                                      UploadQueue<Item>& refinement,
                                      Cost cost,
                                      Adopt adopt,
                                      Elapsed elapsed) {
    UploadBudget budget(kTerrainUploadLimits);
    const auto drain = [&](auto& queue, bool optional) {
        const auto pending = queue.size();
        for (std::size_t visited = 0; visited < pending; ++visited) {
            const auto& item = queue.front();
            const auto bytes = cost(item);
            const auto elapsed_ms = elapsed();
            const auto used = budget.finish(elapsed_ms);
            if (optional && (!std::isfinite(elapsed_ms) || elapsed_ms < 0 ||
                             elapsed_ms >= kTerrainUploadLimits.milliseconds ||
                             used.bytes > kTerrainUploadLimits.bytes ||
                             bytes > kTerrainUploadLimits.bytes - used.bytes)) {
                break;
            }
            if (!budget.admits(bytes, elapsed_ms)) {
                break;
            }
            const bool adopted = adopt(item);
            budget.charge(bytes, adopted);
            queue.complete(adopted);
        }
    };
    drain(baseline, false);
    drain(refinement, true);
    return budget.finish(elapsed());
}

} // namespace Luminumbra::Rendering
