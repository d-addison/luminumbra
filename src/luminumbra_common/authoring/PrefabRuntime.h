#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Authoring {

struct PrefabBounds {
    glm::dvec3 minimum{0.0};
    glm::dvec3 maximum{0.0};
};

struct PrefabMesh {
    std::vector<std::uint8_t> bytes; // Validated, owned LMSH snapshot; no later disk access.
    PrefabBounds bounds;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
};

struct PrefabTexture {
    std::vector<std::uint8_t> bytes; // Complete validated LTEX v1 RGBA8 mip chain.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t mip_levels = 0;
};

struct PrefabMaterial {
    // Exact validated v1 values, including sampling transforms and alpha semantics.
    // This is a runtime binding, not a claim that the current renderer supports it.
    nlohmann::json properties;
    std::map<std::string, std::shared_ptr<const PrefabTexture>> textures;
};

struct PrefabDraw {
    std::string mesh_file;
    std::string material_id;
    std::shared_ptr<const PrefabMesh> mesh;
    std::shared_ptr<const PrefabMaterial> material;
};

struct PrefabNode {
    std::string id;
    std::string label;
    std::string parent_id; // Empty for scene roots.
    std::string mesh_id;   // Empty for transform-only nodes; persistent authored mesh ID.
    glm::dmat4 local_matrix{1.0};
    glm::dmat4 world_matrix{1.0};
    std::vector<PrefabDraw> draws;
};

class PrefabAsset {
public:
    // The manifest digest is required: neither current.json nor arbitrary member paths
    // are followed. Throws on any unsupported contract, corrupt byte or bad reference.
    static std::shared_ptr<const PrefabAsset> Load(const std::filesystem::path& project_root,
                                                   const std::string& generation_id,
                                                   const std::string& manifest_sha256);
    const std::string& asset_id() const {
        return m_asset_id;
    }
    const std::string& generation_id() const {
        return m_generation_id;
    }
    const std::string& manifest_sha256() const {
        return m_manifest_sha256;
    }
    const std::vector<PrefabNode>& nodes() const {
        return m_nodes;
    }

private:
    PrefabAsset() = default;
    std::string m_asset_id;
    std::string m_generation_id;
    std::string m_manifest_sha256;
    std::vector<PrefabNode> m_nodes; // Parent before child; independent of descriptor order.
};

// Separate from TransformComponent: composing rotated nonuniform parents can
// produce shear. Decomposing to TRS would lose the authored geometry placement.
struct PrefabNodeComponent {
    std::string instance_id;
    std::string node_id;
    std::shared_ptr<const PrefabAsset> asset;
    std::size_t node_index = 0;
    glm::dmat4 local_matrix{1.0};
    glm::dmat4 world_matrix{1.0};
    glm::dmat3 normal_matrix{1.0}; // Inverse transpose of the full world linear transform.
    bool reverse_front_face = false;
    std::vector<PrefabBounds> draw_bounds; // World AABBs, one per bound primitive.
};

// Registry must outlive this owner. Instances are local runtime state, not save or
// replication data. Replacement is transactional, including equal-node-count reloads.
// Persistent (instance_id,node_id) identities survive replacement; entt handles do not.
// This owner exclusively manages its entities. Registry callbacks must not reenter
// the scene or mutate its entities, and destruction callbacks must not throw.
class PrefabScene {
public:
    explicit PrefabScene(entt::registry& registry)
        : m_registry(registry) {}
    ~PrefabScene();
    PrefabScene(const PrefabScene&) = delete;
    PrefabScene& operator=(const PrefabScene&) = delete;

    // Reload preserves existing placement unless an explicit new placement is supplied.
    void Replace(const std::string& instance_id, const std::shared_ptr<const PrefabAsset>& asset);
    void Replace(const std::string& instance_id,
                 const std::shared_ptr<const PrefabAsset>& asset,
                 const glm::dmat4& placement);
    bool Remove(const std::string& instance_id);
    // Reads actual ECS components and their immutable asset bindings.
    nlohmann::json Inspect(const std::string& instance_id) const;

private:
    struct Instance {
        std::vector<entt::entity> entities;
        std::uint64_t revision = 0;
        glm::dmat4 placement{1.0};
    };
    void Destroy(const Instance& instance);
    entt::registry& m_registry;
    std::map<std::string, Instance> m_instances;
};

} // namespace Luminumbra::Authoring
