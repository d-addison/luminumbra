// Hierarchical frustum culling system
struct AABB {
    glm::vec3 min;
    glm::vec3 max;

    AABB() = default;
    AABB(const glm::vec3& min, const glm::vec3& max)
        : min(min)
        , max(max) {}
};

struct ChunkCullEntry {
    ChunkID id = 0;
    IVec3 coords{};
    AABB bounds;
};

struct CullingNode {
    AABB bounds;
    std::vector<ChunkCullEntry> chunks;
    std::unique_ptr<CullingNode> children[4]; // Quadtree (X-Z plane)
    bool is_leaf = true;

    CullingNode() = default;
    CullingNode(const AABB& bounds)
        : bounds(bounds) {}
};

class HierarchicalCuller {
public:
    void BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks);
    void CullRecursive(const glm::vec4 frustum_planes[6],
                       CullingNode* node,
                       std::vector<const ChunkCullEntry*>& visible);
    void CullHierarchical(const glm::vec4 frustum_planes[6],
                          std::vector<const ChunkCullEntry*>& visible);
    void Clear();

    std::unique_ptr<CullingNode> m_root; // Made public for access

private:
    static constexpr int MAX_CHUNKS_PER_NODE = 8;
    static constexpr int MAX_DEPTH = 4;

    void BuildRecursive(CullingNode* node, const std::vector<ChunkCullEntry>& chunks, int depth);
    bool AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]);
};
