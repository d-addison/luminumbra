#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace luminumbra::animation {

using MorphMatrix = std::array<float, 16>;
inline constexpr MorphMatrix kMorphIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
inline constexpr std::uint64_t kMorphMaxBytes = 256ull * 1024 * 1024;
inline constexpr std::uint32_t kMorphMaxVertices = 1'000'000;
inline constexpr std::uint32_t kMorphMaxIndices = 3'000'000;
inline constexpr std::uint32_t kMorphMaxTargets = 64;

struct MorphVertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{};
};
struct MorphDelta {
    std::array<float, 3> position{}, normal{};
};
struct MorphTarget {
    std::vector<MorphDelta> vertices; // Same vertex order as the base, without welding.
};

// Mutable compiler input. It cannot be passed directly to the evaluator.
struct MorphMeshData {
    MorphMatrix source_world = kMorphIdentity;
    std::uint32_t source_material_index = UINT32_MAX; // Provenance, not a compiled material.
    std::vector<MorphVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MorphTarget> targets; // Source target index/order is the target identity.
    std::vector<float> default_weights;
};

class MorphMeshAsset {
public:
    MorphMeshAsset(const MorphMeshAsset&) = delete;
    MorphMeshAsset& operator=(const MorphMeshAsset&) = delete;
    MorphMeshAsset(MorphMeshAsset&&) = delete;
    MorphMeshAsset& operator=(MorphMeshAsset&&) = delete;
    const MorphMeshData& Data() const noexcept {
        return m_data;
    }

private:
    explicit MorphMeshAsset(MorphMeshData data);
    MorphMeshData m_data;
    friend bool
    CreateMorphMeshAsset(MorphMeshData, std::shared_ptr<const MorphMeshAsset>&, std::string*);
};

// Every operation replaces out only after complete success. Published assets are
// immutable; failed decode/evaluation retains the caller's previous asset/frame.
bool CreateMorphMeshAsset(MorphMeshData data,
                          std::shared_ptr<const MorphMeshAsset>& out,
                          std::string* error = nullptr);
bool DecodeMorphMeshAsset(std::span<const std::uint8_t> bytes,
                          std::shared_ptr<const MorphMeshAsset>& out,
                          std::string* error = nullptr);
bool LoadMorphMeshAsset(const std::filesystem::path& path,
                        std::shared_ptr<const MorphMeshAsset>& out,
                        std::string* error = nullptr);
bool EncodeMorphMeshAsset(const MorphMeshAsset& asset, std::vector<std::uint8_t>& out);

struct MorphFrame {
    std::shared_ptr<const MorphMeshAsset>
        asset;                         // Retains immutable triangle indices/UV provenance.
    std::vector<MorphVertex> vertices; // Actual float vertices after placement * source_world.
    std::array<float, 3> bounds_min{}, bounds_max{};
    bool reverse_front_face = false; // Caller must reverse front-face interpretation for mirrors.
};

// Weights have exactly target_count entries. Negative/extrapolated finite weights
// are supported without clamping; unrepresentable results/zero normals refuse.
// Morph deltas are applied before either transform. No skinning or GL is involved.
bool EvaluateMorphMesh(const std::shared_ptr<const MorphMeshAsset>& asset,
                       std::span<const float> weights,
                       const MorphMatrix& placement,
                       MorphFrame& out,
                       std::string* error = nullptr);

} // namespace luminumbra::animation
