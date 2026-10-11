#include <luminumbra/rendering/StaticScene.h>

#include "authoring/PrefabRuntime.h"
#include "authoring/PrefabValidation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
using Authoring::Detail::Require;
constexpr std::size_t kByteLimit = 512u * 1024u * 1024u;
constexpr std::size_t kDrawLimit = 65536;
constexpr std::size_t kNodeLimit = 65536;
void GpuNumber(double value) {
    Require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
            "Static scene number is not representable as a finite GPU float");
}
glm::dmat4 Matrix(const StaticMatrix4& values) {
    glm::dmat4 matrix;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            matrix[c][r] = values[static_cast<std::size_t>(c * 4 + r)];
    Authoring::Detail::Affine(matrix);
    return matrix;
}
StaticMatrix4 Matrix(const glm::dmat4& matrix) {
    StaticMatrix4 values;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            const auto value = matrix[c][r];
            GpuNumber(value);
            values[static_cast<std::size_t>(c * 4 + r)] = value;
        }
    return values;
}
StaticBounds Bounds(const Authoring::PrefabBounds& source, const glm::dmat4& matrix) {
    StaticBounds bounds;
    for (unsigned mask = 0; mask < 8; ++mask) {
        glm::dvec4 corner(1);
        for (int axis = 0; axis < 3; ++axis)
            corner[axis] = (mask & (1u << axis)) ? source.maximum[axis] : source.minimum[axis];
        const auto point = matrix * corner;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto value = point[static_cast<int>(axis)];
            GpuNumber(value);
            bounds.minimum[axis] = mask == 0 ? value : std::min(bounds.minimum[axis], value);
            bounds.maximum[axis] = mask == 0 ? value : std::max(bounds.maximum[axis], value);
        }
    }
    return bounds;
}
std::uint32_t Word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated static mesh");
    std::uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i)
        word |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return word;
}
std::shared_ptr<const StaticMesh> DecodeMesh(const Authoring::PrefabMesh& raw,
                                             const std::string& identity) {
    auto mesh = std::make_shared<StaticMesh>();
    mesh->identity = identity;
    mesh->bounds = Bounds(raw.bounds, glm::dmat4(1));
    mesh->vertices.resize(raw.vertex_count);
    for (std::size_t i = 0; i < mesh->vertices.size(); ++i) {
        auto& vertex = mesh->vertices[i];
        for (std::size_t k = 0; k < 8; ++k) {
            const auto value = std::bit_cast<float>(Word(raw.bytes, 28 + i * 32 + k * 4));
            if (k < 3)
                vertex.position[k] = value;
            else if (k < 6)
                vertex.normal[k - 3] = value;
            else
                vertex.uv[k - 6] = value;
        }
        const auto& normal = vertex.normal;
        const double length2 = double(normal[0]) * normal[0] + double(normal[1]) * normal[1] +
                               double(normal[2]) * normal[2];
        Require(length2 > 1e-20 && length2 <= std::numeric_limits<float>::max(),
                "Static geometry has an unusable normal magnitude");
    }
    mesh->indices.resize(raw.index_count);
    for (std::size_t i = 0; i < mesh->indices.size(); ++i)
        mesh->indices[i] = Word(raw.bytes, 28 + std::size_t(raw.vertex_count) * 32 + i * 4);
    return mesh;
}
std::shared_ptr<const StaticTexture> DecodeTexture(const Authoring::PrefabTexture& raw,
                                                   const std::string& identity,
                                                   StaticEncoding encoding) {
    auto texture = std::make_shared<StaticTexture>();
    texture->identity = identity;
    texture->encoding = encoding;
    std::size_t offset = 17;
    auto width = raw.width, height = raw.height;
    for (unsigned i = 0; i < raw.mip_levels; ++i) {
        const std::size_t size = std::size_t(width) * height * 4;
        Require(offset <= raw.bytes.size() && size <= raw.bytes.size() - offset,
                "Incomplete static texture");
        texture->mips.push_back({width,
                                 height,
                                 {raw.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  raw.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)}});
        offset += size;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    Require(offset == raw.bytes.size(), "Trailing static texture data");
    return texture;
}
template<std::size_t N>
std::array<double, N> Values(const nlohmann::json& value) {
    std::array<double, N> result;
    for (std::size_t i = 0; i < N; ++i) {
        result[i] = value.at(i).get<double>();
        GpuNumber(result[i]);
    }
    return result;
}
} // namespace

struct StaticPrefab::Impl {
    std::shared_ptr<const Authoring::PrefabAsset> asset;
    std::map<std::string, std::shared_ptr<const StaticMesh>> meshes;
    std::map<std::string, std::shared_ptr<const StaticTexture>> textures;
    std::map<std::string, std::shared_ptr<const StaticMaterial>> materials;
    std::size_t decoded_bytes = 0;
};
StaticPrefab::StaticPrefab(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
StaticPrefab::~StaticPrefab() = default;
const std::string& StaticPrefab::generation_id() const {
    return m_impl->asset->generation_id();
}
const std::string& StaticPrefab::manifest_sha256() const {
    return m_impl->asset->manifest_sha256();
}
std::shared_ptr<const StaticPrefab> StaticPrefab::Load(const std::string& project_root,
                                                       const std::string& generation_id,
                                                       const std::string& manifest_sha256) {
    auto impl = std::make_unique<Impl>();
    impl->asset = Authoring::PrefabAsset::Load(
        std::filesystem::path(std::u8string(project_root.begin(), project_root.end())),
        generation_id,
        manifest_sha256);
    Require(impl->asset->nodes().size() <= kNodeLimit, "Static prefab node budget exceeded");
    for (const auto& node : impl->asset->nodes()) {
        for (const auto& draw : node.draws) {
            if (!impl->meshes.contains(draw.mesh_file)) {
                const auto& raw = *draw.mesh;
                impl->decoded_bytes += std::size_t(raw.vertex_count) * sizeof(StaticVertex) +
                                       std::size_t(raw.index_count) * sizeof(std::uint32_t);
                Require(impl->decoded_bytes <= kByteLimit,
                        "Static prefab decoded byte budget exceeded");
                impl->meshes.emplace(draw.mesh_file,
                                     DecodeMesh(raw, manifest_sha256 + ":" + draw.mesh_file));
            }
            if (impl->materials.contains(draw.material_id))
                continue;
            const auto& properties = draw.material->properties;
            Require(properties.at("alpha_mode") != "BLEND",
                    "Static preview does not support BLEND");
            auto material = std::make_shared<StaticMaterial>();
            material->identity = manifest_sha256 + ":material:" + draw.material_id;
            material->base_color = Values<4>(properties.at("base_color"));
            material->emissive = Values<3>(properties.at("emissive"));
            material->metallic = properties.at("metallic").get<double>();
            material->roughness = properties.at("roughness").get<double>();
            material->normal_scale = properties.at("normal_scale").get<double>();
            material->occlusion_strength = properties.at("occlusion_strength").get<double>();
            material->alpha_cutoff = properties.at("alpha_cutoff").get<double>();
            material->alpha_mode = properties.at("alpha_mode") == "MASK" ? StaticAlphaMode::Mask
                                                                         : StaticAlphaMode::Opaque;
            material->double_sided = properties.at("double_sided").get<bool>();
            for (double value : {material->metallic,
                                 material->roughness,
                                 material->normal_scale,
                                 material->occlusion_strength,
                                 material->alpha_cutoff})
                GpuNumber(value);
            constexpr std::array<const char*, 5> roles{"baseColorTexture",
                                                       "metallicRoughnessTexture",
                                                       "normalTexture",
                                                       "occlusionTexture",
                                                       "emissiveTexture"};
            for (std::size_t i = 0; i < roles.size(); ++i) {
                const auto& maps = properties.at("textures");
                if (!maps.contains(roles[i]))
                    continue;
                const auto& map = maps.at(roles[i]);
                auto& binding = material->textures[i];
                const auto file = map.at("file").get<std::string>();
                const bool srgb = i == 0 || i == 4;
                const auto key = file + (srgb ? ":srgb" : ":linear");
                if (!impl->textures.contains(key)) {
                    const auto& raw = *draw.material->textures.at(roles[i]);
                    impl->decoded_bytes += raw.bytes.size() - 17;
                    Require(impl->decoded_bytes <= kByteLimit,
                            "Static prefab decoded byte budget exceeded");
                    impl->textures.emplace(
                        key,
                        DecodeTexture(raw,
                                      manifest_sha256 + ":" + key,
                                      srgb ? StaticEncoding::Srgb : StaticEncoding::Linear));
                }
                binding.texture = impl->textures.at(key);
                const auto& sampler = map.at("sampler");
                binding.sampler = {sampler.at("wrapS").get<std::uint32_t>(),
                                   sampler.at("wrapT").get<std::uint32_t>(),
                                   sampler.at("minFilter").get<std::uint32_t>(),
                                   sampler.at("magFilter").get<std::uint32_t>()};
                const auto& coordinates = map.at("coordinates");
                binding.source_uv_set = coordinates.at("set").get<std::uint32_t>();
                binding.offset = Values<2>(coordinates.at("offset"));
                binding.scale = Values<2>(coordinates.at("scale"));
                binding.rotation = coordinates.at("rotation").get<double>();
                GpuNumber(binding.rotation);
            }
            impl->materials.emplace(draw.material_id, std::move(material));
        }
    }
    return std::shared_ptr<const StaticPrefab>(new StaticPrefab(std::move(impl)));
}

struct StaticScene::Impl {
    struct Instance {
        std::shared_ptr<const StaticPrefab> prefab;
        glm::dmat4 placement{1};
        std::map<std::string, glm::dmat4> local_overrides;
    };
    using Instances = std::map<std::string, Instance>;
    struct State {
        entt::registry registry;
        Authoring::PrefabScene scene{registry};
        Instances instances;
        std::shared_ptr<const StaticDrawSnapshot> snapshot = std::make_shared<StaticDrawSnapshot>();
    };
    std::unique_ptr<State> state = std::make_unique<State>();
    void Check(std::uint64_t expected) const {
        Require(expected == state->snapshot->revision, "Stale static scene revision");
        Require(expected < std::numeric_limits<std::uint64_t>::max(),
                "Static scene revision exhausted");
    }
    void Commit(Instances instances) {
        Require(instances.size() <= 64, "Static scene instance budget exceeded");
        auto candidate = std::make_unique<State>();
        candidate->instances = std::move(instances);
        auto snapshot = std::make_shared<StaticDrawSnapshot>();
        snapshot->revision = state->snapshot->revision + 1;
        std::set<const StaticPrefab*> assets;
        std::size_t bytes = 0, nodes = 0;
        for (const auto& [id, instance] : candidate->instances) {
            const auto& prefab = *instance.prefab->m_impl;
            if (assets.insert(instance.prefab.get()).second)
                bytes += prefab.decoded_bytes;
            nodes += prefab.asset->nodes().size();
            Require(bytes <= kByteLimit && nodes <= kNodeLimit,
                    "Static scene resource budget exceeded");
            candidate->scene.Replace(id, prefab.asset, instance.placement);
            std::map<std::string, Authoring::PrefabNodeComponent*> components;
            auto view = candidate->registry.view<Authoring::PrefabNodeComponent>();
            for (auto entity : view) {
                auto& component = view.get<Authoring::PrefabNodeComponent>(entity);
                if (component.instance_id == id)
                    components.emplace(component.node_id, &component);
            }
            for (const auto& node : prefab.asset->nodes()) {
                auto& component = *components.at(node.id);
                const auto found = instance.local_overrides.find(node.id);
                component.local_matrix =
                    found == instance.local_overrides.end() ? node.local_matrix : found->second;
                component.world_matrix =
                    (node.parent_id.empty() ? instance.placement
                                            : components.at(node.parent_id)->world_matrix) *
                    component.local_matrix;
                Authoring::Detail::Affine(component.world_matrix);
                component.normal_matrix =
                    glm::transpose(glm::inverse(glm::dmat3(component.world_matrix)));
                component.reverse_front_face =
                    glm::determinant(glm::dmat3(component.world_matrix)) < 0;
                const auto model = Matrix(component.world_matrix);
                StaticMatrix3 normal;
                for (int c = 0; c < 3; ++c)
                    for (int r = 0; r < 3; ++r) {
                        const auto value = component.normal_matrix[c][r];
                        GpuNumber(value);
                        normal[static_cast<std::size_t>(c * 3 + r)] = value;
                    }
                for (std::size_t i = 0; i < node.draws.size(); ++i) {
                    Require(snapshot->draws.size() < kDrawLimit,
                            "Static scene draw budget exceeded");
                    const auto& draw = node.draws[i];
                    component.draw_bounds[i] =
                        {}; // ECS owns the same transformed bounds as the snapshot.
                    const auto bounds = Bounds(draw.mesh->bounds, component.world_matrix);
                    for (int axis = 0; axis < 3; ++axis) {
                        component.draw_bounds[i].minimum[axis] =
                            bounds.minimum[static_cast<std::size_t>(axis)];
                        component.draw_bounds[i].maximum[axis] =
                            bounds.maximum[static_cast<std::size_t>(axis)];
                    }
                    snapshot->draws.push_back({{id, node.id, static_cast<std::uint32_t>(i)},
                                               draw.material_id,
                                               instance.prefab,
                                               prefab.meshes.at(draw.mesh_file),
                                               prefab.materials.at(draw.material_id),
                                               model,
                                               normal,
                                               component.reverse_front_face,
                                               bounds});
                }
            }
        }
        candidate->snapshot = std::move(snapshot);
        state.swap(candidate); // All throwing work completed; no external ECS callbacks exist.
    }
};
StaticScene::StaticScene()
    : m_impl(std::make_unique<Impl>()) {}
StaticScene::~StaticScene() = default;
std::shared_ptr<const StaticDrawSnapshot> StaticScene::Snapshot() const {
    return m_impl->state->snapshot;
}
void StaticScene::Replace(const std::string& instance_id,
                          std::shared_ptr<const StaticPrefab> prefab,
                          const StaticMatrix4& placement,
                          std::uint64_t expected_revision) {
    m_impl->Check(expected_revision);
    Require(Authoring::Detail::Identifier(instance_id) && prefab != nullptr,
            "Invalid static scene instance");
    auto instances = m_impl->state->instances;
    instances[instance_id] = {std::move(prefab), Matrix(placement), {}};
    m_impl->Commit(std::move(instances));
}
void StaticScene::UpdateLocalMatrices(std::span<const StaticLocalMatrixUpdate> updates,
                                      std::uint64_t expected_revision) {
    m_impl->Check(expected_revision);
    Require(updates.size() <= kNodeLimit, "Static scene update budget exceeded");
    auto instances = m_impl->state->instances;
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& update : updates) {
        Require(unique.emplace(update.instance_id, update.node_id).second,
                "Duplicate static node update");
        const auto found = instances.find(update.instance_id);
        Require(found != instances.end(), "Unknown static instance update");
        auto& instance = found->second;
        const auto& nodes = instance.prefab->m_impl->asset->nodes();
        Require(std::any_of(nodes.begin(),
                            nodes.end(),
                            [&](const auto& node) { return node.id == update.node_id; }),
                "Unknown static node update");
        instance.local_overrides[update.node_id] = Matrix(update.local);
    }
    if (!updates.empty())
        m_impl->Commit(std::move(instances));
}
void StaticScene::Remove(const std::string& instance_id, std::uint64_t expected_revision) {
    m_impl->Check(expected_revision);
    auto instances = m_impl->state->instances;
    Require(instances.erase(instance_id) == 1, "Unknown static instance removal");
    m_impl->Commit(std::move(instances));
}
void StaticScene::ReplaceAll(std::span<const StaticInstanceDescription> descriptions,
                             std::uint64_t expected_revision) {
    m_impl->Check(expected_revision);
    Require(descriptions.size() <= 64, "Static scene instance budget exceeded");
    Impl::Instances instances;
    for (const auto& description : descriptions) {
        Require(Authoring::Detail::Identifier(description.instance_id) && description.prefab,
                "Invalid complete static instance");
        const auto& nodes = description.prefab->m_impl->asset->nodes();
        Require(description.locals.size() == nodes.size(), "Incomplete static node set");
        Impl::Instance instance{description.prefab, Matrix(description.placement), {}};
        for (const auto& local : description.locals)
            Require(instance.local_overrides.emplace(local.node_id, Matrix(local.local)).second,
                    "Duplicate complete static node");
        for (const auto& node : nodes)
            Require(instance.local_overrides.contains(node.id), "Unknown complete static node");
        Require(instances.emplace(description.instance_id, std::move(instance)).second,
                "Duplicate complete static instance");
    }
    m_impl->Commit(std::move(instances));
}
} // namespace Luminumbra::Rendering
