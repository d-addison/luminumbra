#include "PrefabRuntime.h"

#include "PrefabValidation.h"
#include "components/CoreComponents.h"

#include <limits>
#include <utility>

namespace Luminumbra::Authoring {
namespace {
PrefabBounds WorldBounds(const PrefabBounds& bounds, const glm::dmat4& matrix) {
    PrefabBounds result;
    for (unsigned mask = 0; mask < 8; ++mask) {
        glm::dvec4 corner(1.0);
        for (unsigned axis = 0; axis < 3; ++axis)
            corner[axis] = (mask & (1u << axis)) ? bounds.maximum[axis] : bounds.minimum[axis];
        const glm::dvec3 point(matrix * corner);
        for (int axis = 0; axis < 3; ++axis)
            Detail::Require(std::isfinite(point[axis]), "Prefab world bound is nonfinite");
        result.minimum = mask == 0 ? point : glm::min(result.minimum, point);
        result.maximum = mask == 0 ? point : glm::max(result.maximum, point);
    }
    return result;
}

nlohmann::json BoundsJson(const PrefabBounds& bounds) {
    return {{"minimum", {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z}},
            {"maximum", {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}}};
}
} // namespace

void PrefabScene::Destroy(const Instance& instance) {
    for (const auto entity : instance.entities)
        if (m_registry.valid(entity))
            m_registry.destroy(entity);
}

PrefabScene::~PrefabScene() {
    for (const auto& [id, instance] : m_instances) {
        (void)id;
        Destroy(instance);
    }
}

void PrefabScene::Replace(const std::string& instance_id,
                          const std::shared_ptr<const PrefabAsset>& asset) {
    const auto old = m_instances.find(instance_id);
    const auto placement = old == m_instances.end() ? glm::dmat4(1.0) : old->second.placement;
    Replace(instance_id, asset, placement);
}

void PrefabScene::Replace(const std::string& instance_id,
                          const std::shared_ptr<const PrefabAsset>& asset,
                          const glm::dmat4& placement) {
    Detail::Require(Detail::Identifier(instance_id), "Invalid persistent prefab instance ID");
    Detail::Require(asset != nullptr, "Missing prefab asset");
    Detail::Affine(placement);
    const auto old = m_instances.find(instance_id);
    Detail::Require(old == m_instances.end() ||
                        old->second.revision < std::numeric_limits<std::uint64_t>::max(),
                    "Prefab instance revision exhausted");
    Instance candidate;
    candidate.placement = placement;
    candidate.revision = old == m_instances.end() ? 1 : old->second.revision + 1;
    candidate.entities.reserve(asset->nodes().size());
    std::vector<PrefabNodeComponent> components;
    components.reserve(asset->nodes().size());
    for (std::size_t i = 0; i < asset->nodes().size(); ++i) {
        const auto& node = asset->nodes()[i];
        PrefabNodeComponent component;
        component.instance_id = instance_id;
        component.node_id = node.id;
        component.asset = asset;
        component.node_index = i;
        component.local_matrix = node.local_matrix;
        component.world_matrix = placement * node.world_matrix;
        Detail::Affine(component.world_matrix);
        component.normal_matrix = glm::transpose(glm::inverse(glm::dmat3(component.world_matrix)));
        component.reverse_front_face = glm::determinant(glm::dmat3(component.world_matrix)) < 0;
        component.draw_bounds.reserve(node.draws.size());
        for (const auto& draw : node.draws)
            component.draw_bounds.push_back(WorldBounds(draw.mesh->bounds, component.world_matrix));
        components.push_back(std::move(component));
    }
    // Create a complete separate candidate. Component allocation or user-provided
    // registry construction callbacks can throw without discarding the old instance.
    try {
        std::map<std::string, entt::entity> entities;
        for (std::size_t i = 0; i < components.size(); ++i) {
            const auto entity = m_registry.create();
            candidate.entities.push_back(entity);
            const auto& node = asset->nodes()[i];
            entities.emplace(node.id, entity);
            m_registry.emplace<PrefabNodeComponent>(entity, std::move(components[i]));
            m_registry.emplace<Components::TagComponent>(entity, node.label);
            auto& hierarchy = m_registry.emplace<Components::HierarchyComponent>(entity);
            if (!node.parent_id.empty()) {
                hierarchy.parent = entities.at(node.parent_id);
                m_registry.get<Components::HierarchyComponent>(hierarchy.parent)
                    .children.push_back(entity);
            }
        }
        if (old == m_instances.end()) {
            // Allocate the map entry before transferring ownership of candidate entities.
            auto [it, inserted] = m_instances.try_emplace(instance_id);
            Detail::Require(inserted, "Prefab instance was concurrently changed");
            it->second = std::move(candidate);
        } else {
            std::swap(old->second, candidate);
            Destroy(candidate);
        }
    } catch (...) {
        Destroy(candidate);
        throw;
    }
}

bool PrefabScene::Remove(const std::string& instance_id) {
    const auto it = m_instances.find(instance_id);
    if (it == m_instances.end())
        return false;
    Destroy(it->second);
    m_instances.erase(it);
    return true;
}

nlohmann::json PrefabScene::Inspect(const std::string& instance_id) const {
    const auto& instance = m_instances.at(instance_id);
    auto result = nlohmann::json{{"schema", "luminumbra.prefab.instance.v1"},
                                 {"instance_id", instance_id},
                                 {"revision", instance.revision},
                                 {"renderer_qualified", false},
                                 {"placement_matrix", Detail::MatrixJson(instance.placement)},
                                 {"runtime_components", false},
                                 {"nodes", nlohmann::json::array()},
                                 {"meshes", nlohmann::json::object()},
                                 {"materials", nlohmann::json::object()}};
    for (const auto entity : instance.entities) {
        const auto& component = m_registry.get<PrefabNodeComponent>(entity);
        const auto& hierarchy = m_registry.get<Components::HierarchyComponent>(entity);
        const auto& node = component.asset->nodes().at(component.node_index);
        result["asset_id"] = component.asset->asset_id();
        result["generation_id"] = component.asset->generation_id();
        result["manifest_sha256"] = component.asset->manifest_sha256();
        auto normal = nlohmann::json::array();
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                normal.push_back(component.normal_matrix[c][r]);
        auto record = nlohmann::json{
            {"id", component.node_id},
            {"label", m_registry.get<Components::TagComponent>(entity).tag},
            {"parent",
             hierarchy.parent == entt::null
                 ? nlohmann::json(nullptr)
                 : nlohmann::json(m_registry.get<PrefabNodeComponent>(hierarchy.parent).node_id)},
            {"local_matrix", Detail::MatrixJson(component.local_matrix)},
            {"mesh", node.mesh_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(node.mesh_id)},
            {"world_matrix", Detail::MatrixJson(component.world_matrix)},
            {"normal_matrix", normal},
            {"reverse_front_face", component.reverse_front_face},
            {"children", nlohmann::json::array()},
            {"draws", nlohmann::json::array()}};
        for (const auto child : hierarchy.children)
            record["children"].push_back(m_registry.get<PrefabNodeComponent>(child).node_id);
        for (std::size_t i = 0; i < node.draws.size(); ++i) {
            const auto& draw = node.draws[i];
            record["draws"].push_back({{"mesh", draw.mesh_file},
                                       {"material", draw.material_id},
                                       {"world_bounds", BoundsJson(component.draw_bounds.at(i))}});
            result["meshes"][draw.mesh_file] = {{"vertices", draw.mesh->vertex_count},
                                                {"indices", draw.mesh->index_count},
                                                {"bytes", draw.mesh->bytes.size()},
                                                {"local_bounds", BoundsJson(draw.mesh->bounds)}};
            result["materials"][draw.material_id] = draw.material->properties;
        }
        result["nodes"].push_back(std::move(record));
    }
    return result;
}
} // namespace Luminumbra::Authoring
