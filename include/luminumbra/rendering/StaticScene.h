#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

using StaticMatrix4 = std::array<double, 16>; // Column-major full affine matrix.
using StaticMatrix3 = std::array<double, 9>;
inline constexpr StaticMatrix4 kStaticIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
struct StaticBounds {
    std::array<double, 3> minimum{}, maximum{};
};
struct StaticVertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{}; // Compiler-selected raw UVs; not transformed again at load.
};
struct StaticMesh {
    std::string identity; // Manifest digest + immutable member name.
    std::vector<StaticVertex> vertices;
    std::vector<std::uint32_t> indices;
    StaticBounds bounds;
};
enum class StaticEncoding {
    Linear,
    Srgb
};
struct StaticMip {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba8;
};
struct StaticTexture {
    std::string identity; // Includes generation manifest, member and encoding.
    StaticEncoding encoding = StaticEncoding::Linear;
    std::vector<StaticMip> mips; // Exact complete authored chain; no regeneration.
};
struct StaticSampler {
    std::uint32_t wrap_s = 10497, wrap_t = 10497;
    std::uint32_t min_filter = 9987, mag_filter = 9729; // Validated glTF sampler values.
};
struct StaticTextureBinding {
    std::shared_ptr<const StaticTexture> texture; // Null means role-specific fallback.
    StaticSampler sampler;
    std::uint32_t source_uv_set = 0; // Provenance: already selected in StaticVertex::uv.
    std::array<double, 2> offset{0, 0}, scale{1, 1};
    double rotation = 0; // Sampling = offset + rotation(scale * raw_uv).
};
enum class StaticTextureRole : std::size_t {
    BaseColor,
    MetallicRoughness,
    Normal,
    Occlusion,
    Emissive
};
enum class StaticAlphaMode {
    Opaque,
    Mask
};
struct StaticMaterial {
    std::string identity;
    std::array<double, 4> base_color{1, 1, 1, 1};
    std::array<double, 3> emissive{0, 0, 0};
    double metallic = 1, roughness = 1, normal_scale = 1, occlusion_strength = 1;
    StaticAlphaMode alpha_mode = StaticAlphaMode::Opaque;
    double alpha_cutoff = .5;
    bool double_sided = false;
    std::array<StaticTextureBinding, 5> textures;
};

// Immutable validated generation, loaded by the real pinned PrefabAsset reader.
// project_root is UTF-8. Initial rendering profile refuses BLEND.
// No file access after successful load.
class StaticPrefab {
public:
    static std::shared_ptr<const StaticPrefab> Load(const std::string& project_root,
                                                    const std::string& generation_id,
                                                    const std::string& manifest_sha256);
    ~StaticPrefab();
    const std::string& generation_id() const;
    const std::string& manifest_sha256() const;

private:
    struct Impl;
    explicit StaticPrefab(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
    friend class StaticScene;
};
struct StaticDrawKey {
    std::string instance_id, node_id;
    std::uint32_t primitive_index = 0;
    bool operator==(const StaticDrawKey&) const = default;
};
struct StaticDraw {
    StaticDrawKey key;
    std::string material_id;
    std::shared_ptr<const StaticPrefab> generation;
    std::shared_ptr<const StaticMesh> mesh;
    std::shared_ptr<const StaticMaterial> material;
    StaticMatrix4 model = kStaticIdentity;
    StaticMatrix3 normal{}; // Inverse transpose of model linear part, in world space.
    bool reverse_front_face = false;
    StaticBounds world_bounds;
};
struct StaticDrawSnapshot {
    std::uint64_t revision = 0;
    std::vector<StaticDraw>
        draws; // Stable instance ID, authored parent-first node, primitive order.
};
struct StaticLocalMatrixUpdate {
    std::string instance_id, node_id;
    StaticMatrix4 local = kStaticIdentity;
};
struct StaticNodeMatrix {
    std::string node_id;
    StaticMatrix4 local = kStaticIdentity;
};
struct StaticInstanceDescription {
    std::string instance_id;
    std::shared_ptr<const StaticPrefab> prefab;
    StaticMatrix4 placement = kStaticIdentity;
    std::vector<StaticNodeMatrix> locals; // Every descriptor node exactly once.
};

// Single owning thread. Snapshots/resources are immutable and can outlive the scene.
// Every mutation requires the current revision; failure preserves snapshot identity.
// No GL, file watching, callbacks, or geometry rebuild occurs in local-matrix updates.
class StaticScene {
public:
    StaticScene();
    ~StaticScene();
    StaticScene(const StaticScene&) = delete;
    StaticScene& operator=(const StaticScene&) = delete;
    std::shared_ptr<const StaticDrawSnapshot> Snapshot() const;
    void Replace(const std::string& instance_id,
                 std::shared_ptr<const StaticPrefab> prefab,
                 const StaticMatrix4& placement,
                 std::uint64_t expected_revision);
    void UpdateLocalMatrices(std::span<const StaticLocalMatrixUpdate> updates,
                             std::uint64_t expected_revision);
    void Remove(const std::string& instance_id, std::uint64_t expected_revision);
    // Complete replacement, including removals, commits once after all instances
    // and exact node membership validate. Decoded immutable resources are reused.
    void ReplaceAll(std::span<const StaticInstanceDescription> instances,
                    std::uint64_t expected_revision);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Luminumbra::Rendering
