#include "MorphMesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <utility>

namespace luminumbra::animation {
namespace {
constexpr std::uint32_t kMagic = 0x524f4d4c; // LMOR, little endian.
constexpr std::uint32_t kVersion = 1;
using Matrix = std::array<double, 16>;
using NormalMatrix = std::array<double, 9>;

bool Fail(std::string* error, const char* message) {
    if (error)
        *error = message;
    return false;
}

template<typename Values>
bool Finite(const Values& values) {
    return std::all_of(
        values.begin(), values.end(), [](auto value) { return std::isfinite(value); });
}

template<typename Values>
bool Affine(const Values& values) {
    return Finite(values) && values[3] == 0 && values[7] == 0 && values[11] == 0 && values[15] == 1;
}

bool NormalTransform(const Matrix& m, NormalMatrix& normal, bool& mirrored) {
    if (!Affine(m))
        return false;
    for (std::size_t col = 0; col < 3; ++col) {
        const std::size_t a = (col + 1) % 3, b = (col + 2) % 3;
        normal[col * 3] = m[a * 4 + 1] * m[b * 4 + 2] - m[a * 4 + 2] * m[b * 4 + 1];
        normal[col * 3 + 1] = m[a * 4 + 2] * m[b * 4] - m[a * 4] * m[b * 4 + 2];
        normal[col * 3 + 2] = m[a * 4] * m[b * 4 + 1] - m[a * 4 + 1] * m[b * 4];
    }
    const double determinant = m[0] * normal[0] + m[1] * normal[1] + m[2] * normal[2];
    if (!std::isfinite(determinant) || determinant == 0)
        return false;
    mirrored = determinant < 0;
    for (double& value : normal)
        value /= determinant;
    return Finite(normal);
}

bool Extent(std::uint64_t vertices,
            std::uint64_t indices,
            std::uint64_t targets,
            std::uint64_t& bytes) {
    if (!vertices || vertices > kMorphMaxVertices || indices < 3 || indices > kMorphMaxIndices ||
        indices % 3 || !targets || targets > kMorphMaxTargets)
        return false;
    // Every factor is bounded above before this arithmetic.
    bytes = 96 + 4 * targets + 32 * vertices + 4 * indices + 24 * vertices * targets;
    return bytes <= kMorphMaxBytes;
}

bool Validate(const MorphMeshData& data, std::string* error) {
    std::uint64_t bytes = 0;
    if (!Extent(data.vertices.size(), data.indices.size(), data.targets.size(), bytes) ||
        data.default_weights.size() != data.targets.size() || !Finite(data.default_weights))
        return Fail(error, "Invalid morph counts, weights or byte budget");
    Matrix world{};
    std::copy(data.source_world.begin(), data.source_world.end(), world.begin());
    NormalMatrix normal{};
    bool mirrored = false;
    if (!NormalTransform(world, normal, mirrored))
        return Fail(error, "Morph source transform must be finite, affine and nonsingular");
    for (const auto& vertex : data.vertices)
        if (!Finite(vertex.position) || !Finite(vertex.normal) || !Finite(vertex.uv) ||
            std::hypot(
                double(vertex.normal[0]), double(vertex.normal[1]), double(vertex.normal[2])) == 0)
            return Fail(error, "Invalid morph base vertex or zero normal");
    for (auto index : data.indices)
        if (index >= data.vertices.size())
            return Fail(error, "Morph index exceeds base vertex count");
    for (const auto& target : data.targets) {
        if (target.vertices.size() != data.vertices.size())
            return Fail(error, "Morph target count differs from the base topology");
        for (const auto& delta : target.vertices)
            if (!Finite(delta.position) || !Finite(delta.normal))
                return Fail(error, "Nonfinite morph delta");
    }
    return true;
}

void U32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= std::uint32_t(bytes[offset++]) << shift;
    return value;
}

template<typename Values>
void Floats(std::vector<std::uint8_t>& bytes, const Values& values) {
    for (float value : values)
        U32(bytes, std::bit_cast<std::uint32_t>(value));
}

template<typename Values>
void ReadFloats(std::span<const std::uint8_t> bytes, std::size_t& offset, Values& values) {
    for (float& value : values)
        value = std::bit_cast<float>(ReadU32(bytes, offset));
}
} // namespace

MorphMeshAsset::MorphMeshAsset(MorphMeshData data)
    : m_data(std::move(data)) {}

bool CreateMorphMeshAsset(MorphMeshData data,
                          std::shared_ptr<const MorphMeshAsset>& out,
                          std::string* error) {
    if (!Validate(data, error))
        return false;
    auto candidate = std::shared_ptr<const MorphMeshAsset>(new MorphMeshAsset(std::move(data)));
    out = std::move(candidate);
    return true;
}

bool EncodeMorphMeshAsset(const MorphMeshAsset& asset, std::vector<std::uint8_t>& out) {
    const auto& data = asset.Data();
    std::uint64_t extent = 0;
    if (!Extent(data.vertices.size(), data.indices.size(), data.targets.size(), extent))
        return false;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(extent));
    for (auto value : {kMagic,
                       kVersion,
                       0u,
                       static_cast<std::uint32_t>(data.vertices.size()),
                       static_cast<std::uint32_t>(data.indices.size()),
                       static_cast<std::uint32_t>(data.targets.size()),
                       data.source_material_index,
                       0u})
        U32(bytes, value);
    Floats(bytes, data.source_world);
    Floats(bytes, data.default_weights);
    for (const auto& vertex : data.vertices) {
        Floats(bytes, vertex.position);
        Floats(bytes, vertex.normal);
        Floats(bytes, vertex.uv);
    }
    for (auto index : data.indices)
        U32(bytes, index);
    for (const auto& target : data.targets)
        for (const auto& delta : target.vertices) {
            Floats(bytes, delta.position);
            Floats(bytes, delta.normal);
        }
    out = std::move(bytes);
    return true;
}

bool DecodeMorphMeshAsset(std::span<const std::uint8_t> bytes,
                          std::shared_ptr<const MorphMeshAsset>& out,
                          std::string* error) {
    if (bytes.size() < 96 || bytes.size() > kMorphMaxBytes)
        return Fail(error, "Truncated or oversized LMOR asset");
    std::size_t offset = 0;
    if (ReadU32(bytes, offset) != kMagic || ReadU32(bytes, offset) != kVersion ||
        ReadU32(bytes, offset) != 0)
        return Fail(error, "Unsupported LMOR magic, version or flags");
    const auto vertices = ReadU32(bytes, offset), indices = ReadU32(bytes, offset),
               targets = ReadU32(bytes, offset);
    MorphMeshData data;
    data.source_material_index = ReadU32(bytes, offset);
    std::uint64_t extent = 0;
    if (ReadU32(bytes, offset) != 0 || !Extent(vertices, indices, targets, extent) ||
        extent != bytes.size())
        return Fail(error, "Invalid LMOR counts, reserved field or exact byte extent");
    ReadFloats(bytes, offset, data.source_world);
    data.default_weights.resize(targets);
    ReadFloats(bytes, offset, data.default_weights);
    data.vertices.resize(vertices);
    for (auto& vertex : data.vertices) {
        ReadFloats(bytes, offset, vertex.position);
        ReadFloats(bytes, offset, vertex.normal);
        ReadFloats(bytes, offset, vertex.uv);
    }
    data.indices.resize(indices);
    for (auto& index : data.indices)
        index = ReadU32(bytes, offset);
    data.targets.resize(targets);
    for (auto& target : data.targets) {
        target.vertices.resize(vertices);
        for (auto& delta : target.vertices) {
            ReadFloats(bytes, offset, delta.position);
            ReadFloats(bytes, offset, delta.normal);
        }
    }
    return CreateMorphMeshAsset(std::move(data), out, error);
}

bool LoadMorphMeshAsset(const std::filesystem::path& path,
                        std::shared_ptr<const MorphMeshAsset>& out,
                        std::string* error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 0 || static_cast<std::uint64_t>(size) > kMorphMaxBytes)
        return Fail(error, "Cannot read bounded LMOR file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return Fail(error, "LMOR file changed or could not be read completely");
    return DecodeMorphMeshAsset(bytes, out, error);
}

bool EvaluateMorphMesh(const std::shared_ptr<const MorphMeshAsset>& asset,
                       std::span<const float> weights,
                       const MorphMatrix& placement,
                       MorphFrame& out,
                       std::string* error) {
    if (!asset || !Affine(placement))
        return Fail(error, "Missing morph asset or invalid placement");
    const auto& data = asset->Data();
    if (weights.size() != data.targets.size() || !Finite(weights))
        return Fail(error, "Supply exactly one finite weight per morph target");
    Matrix world{};
    for (std::size_t col = 0; col < 4; ++col)
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t k = 0; k < 4; ++k)
                world[col * 4 + row] +=
                    double(placement[k * 4 + row]) * data.source_world[col * 4 + k];
    NormalMatrix normal_matrix{};
    MorphFrame candidate;
    if (!NormalTransform(world, normal_matrix, candidate.reverse_front_face))
        return Fail(error, "Combined morph placement is singular or unrepresentable");
    candidate.asset = asset;
    candidate.vertices.resize(data.vertices.size());
    for (std::size_t v = 0; v < data.vertices.size(); ++v) {
        std::array<double, 3> position{}, normal{};
        for (std::size_t c = 0; c < 3; ++c) {
            position[c] = data.vertices[v].position[c];
            normal[c] = data.vertices[v].normal[c];
            for (std::size_t t = 0; t < weights.size(); ++t) {
                position[c] += double(weights[t]) * data.targets[t].vertices[v].position[c];
                normal[c] += double(weights[t]) * data.targets[t].vertices[v].normal[c];
            }
        }
        const double length = std::hypot(normal[0], normal[1], normal[2]);
        if (!std::isfinite(length) || length == 0)
            return Fail(error, "Morph weights produce a zero or invalid normal");
        for (auto& value : normal)
            value /= length;
        auto& vertex = candidate.vertices[v];
        vertex.uv = data.vertices[v].uv;
        std::array<double, 3> transformed_normal{};
        for (std::size_t row = 0; row < 3; ++row) {
            double component = world[12 + row];
            for (std::size_t k = 0; k < 3; ++k) {
                component += world[k * 4 + row] * position[k];
                transformed_normal[row] += normal_matrix[k * 3 + row] * normal[k];
            }
            if (!std::isfinite(component) ||
                std::abs(component) > std::numeric_limits<float>::max())
                return Fail(error, "Morphed position is not representable as float32");
            vertex.position[row] = static_cast<float>(component);
        }
        const double transformed_length =
            std::hypot(transformed_normal[0], transformed_normal[1], transformed_normal[2]);
        if (!std::isfinite(transformed_length) || transformed_length == 0)
            return Fail(error, "Morphed normal transform is unrepresentable");
        for (std::size_t c = 0; c < 3; ++c) {
            vertex.normal[c] = static_cast<float>(transformed_normal[c] / transformed_length);
            candidate.bounds_min[c] =
                v ? std::min(candidate.bounds_min[c], vertex.position[c]) : vertex.position[c];
            candidate.bounds_max[c] =
                v ? std::max(candidate.bounds_max[c], vertex.position[c]) : vertex.position[c];
        }
    }
    out = std::move(candidate);
    return true;
}
} // namespace luminumbra::animation
