#include "../RenderPipeline.h"

#include <limits>

namespace Luminumbra::Rendering {

// --- HIERARCHICAL CULLING IMPLEMENTATION ---

void RenderPipeline::HierarchicalCuller::BuildHierarchy(
    const std::vector<ChunkMeshSnapshot>& chunks) {
    if (chunks.empty()) {
        m_root.reset();
        return;
    }

    std::vector<ChunkCullEntry> chunk_refs;
    chunk_refs.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        glm::vec3 chunk_min(static_cast<float>(chunk.coords.x * CHUNK_SIZE_X),
                            static_cast<float>(chunk.coords.y * CHUNK_SIZE_Y),
                            static_cast<float>(chunk.coords.z * CHUNK_SIZE_Z));
        glm::vec3 chunk_max = chunk_min + glm::vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        chunk_refs.push_back(ChunkCullEntry{chunk.id, chunk.coords, AABB(chunk_min, chunk_max)});
    }

    // Calculate root bounding box from all chunks
    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto& chunk : chunk_refs) {
        min = glm::min(min, chunk.bounds.min);
        max = glm::max(max, chunk.bounds.max);
    }

    m_root = std::make_unique<CullingNode>(AABB(min, max));
    BuildRecursive(m_root.get(), chunk_refs, 0);
}

void RenderPipeline::HierarchicalCuller::BuildRecursive(CullingNode* node,
                                                        const std::vector<ChunkCullEntry>& chunks,
                                                        int depth) {
    // Base cases: too few chunks or maximum depth reached
    if (chunks.size() <= MAX_CHUNKS_PER_NODE || depth >= MAX_DEPTH) {
        node->chunks = chunks;
        node->is_leaf = true;
        return;
    }

    // Split the node into 4 quadrants (X-Z plane)
    glm::vec3 center = (node->bounds.min + node->bounds.max) * 0.5f;

    // Create child bounds
    AABB child_bounds[4] = {
        AABB(node->bounds.min, glm::vec3(center.x, node->bounds.max.y, center.z)), // Bottom-left
        AABB(glm::vec3(center.x, node->bounds.min.y, node->bounds.min.z),
             glm::vec3(node->bounds.max.x, node->bounds.max.y, center.z)), // Bottom-right
        AABB(glm::vec3(node->bounds.min.x, node->bounds.min.y, center.z),
             glm::vec3(center.x, node->bounds.max.y, node->bounds.max.z)),        // Top-left
        AABB(glm::vec3(center.x, node->bounds.min.y, center.z), node->bounds.max) // Top-right
    };

    // Distribute chunks to children
    std::vector<std::vector<ChunkCullEntry>> child_chunks(4);

    for (const auto& chunk : chunks) {
        glm::ivec3 coords = chunk.coords;
        glm::vec3 chunk_center(coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                               coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                               coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f);

        int child_index = 0;
        if (chunk_center.x >= center.x)
            child_index += 1;
        if (chunk_center.z >= center.z)
            child_index += 2;

        child_chunks[child_index].push_back(chunk);
    }

    // Create children for non-empty quadrants
    node->is_leaf = false;
    for (int i = 0; i < 4; ++i) {
        if (!child_chunks[i].empty()) {
            node->children[i] = std::make_unique<CullingNode>(child_bounds[i]);
            BuildRecursive(node->children[i].get(), child_chunks[i], depth + 1);
        }
    }
}

void RenderPipeline::HierarchicalCuller::CullRecursive(
    const glm::vec4 frustum_planes[6],
    CullingNode* node,
    std::vector<const ChunkCullEntry*>& visible) {
    if (!node)
        return;

    // Test this node's bounding box against the frustum
    if (AABBFrustumCulled(node->bounds, frustum_planes)) {
        return; // Entire subtree is culled
    }

    if (node->is_leaf) {
        for (const auto& chunk : node->chunks) {
            if (!AABBFrustumCulled(chunk.bounds, frustum_planes)) {
                visible.push_back(&chunk);
            }
        }
    } else {
        // Recursively test children
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                CullRecursive(frustum_planes, node->children[i].get(), visible);
            }
        }
    }
}

bool RenderPipeline::HierarchicalCuller::AABBFrustumCulled(const AABB& aabb,
                                                           const glm::vec4 frustum_planes[6]) {
    for (int i = 0; i < 6; ++i) {
        glm::vec3 p = glm::vec3(frustum_planes[i].x >= 0 ? aabb.max.x : aabb.min.x,
                                frustum_planes[i].y >= 0 ? aabb.max.y : aabb.min.y,
                                frustum_planes[i].z >= 0 ? aabb.max.z : aabb.min.z);
        if ((glm::dot(glm::vec3(frustum_planes[i]), p) + frustum_planes[i].w) < 0.0f) {
            return true; // Outside this plane
        }
    }
    return false; // Inside all planes
}

void RenderPipeline::HierarchicalCuller::CullHierarchical(
    const glm::vec4 frustum_planes[6], std::vector<const ChunkCullEntry*>& visible) {
    if (m_root) {
        CullRecursive(frustum_planes, m_root.get(), visible);
    }
}

void RenderPipeline::HierarchicalCuller::Clear() {
    m_root.reset();
}

// --- END HIERARCHICAL CULLING ---

} // namespace Luminumbra::Rendering
