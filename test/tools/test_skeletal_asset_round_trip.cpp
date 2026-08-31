// .lmesh v2 (LMS2) +.lanim round-trip coverage.
//
// Builds a tiny rigged glTF (two joints, skinned triangle, one animation with
// rotation + translation channels) programmatically, runs the asset processor
// import, reloads the emitted LMS2 +.lanim files and asserts field-level
// equality against the authored fixture. A double-run bitwise determinism
// check guards the writer.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "luminumbra_common/animation/SkinnedMeshFormat.h"

void process_gltf(const std::string& input_path, const std::string& output_path);

namespace {

using luminumbra::animation::AnimClipAsset;
using luminumbra::animation::AnimTargetType;
using luminumbra::animation::AnimTrack;
using luminumbra::animation::HashJointName;
using luminumbra::animation::kLanimMagic;
using luminumbra::animation::kLms2Magic;
using luminumbra::animation::LoadAnimClipAsset;
using luminumbra::animation::LoadSkinnedMeshAsset;
using luminumbra::animation::SkinnedMeshAsset;
using luminumbra::animation::SkinnedVertexData;

struct BufferView {
    size_t offset;
    size_t length;
};

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("luminumbra_skeletal_round_trip_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

template<typename T>
BufferView AppendValues(std::vector<unsigned char>& buffer, const std::vector<T>& values) {
    while ((buffer.size() % 4) != 0) {
        buffer.push_back(0);
    }

    const size_t offset = buffer.size();
    const size_t length = values.size() * sizeof(T);
    buffer.resize(offset + length);
    std::memcpy(buffer.data() + offset, values.data(), length);
    return {offset, length};
}

std::string Base64Encode(const std::vector<unsigned char>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);

    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t octet0 = bytes[i];
        const uint32_t octet1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
        const uint32_t octet2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
        const uint32_t triple = (octet0 << 16) | (octet1 << 8) | octet2;

        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back((i + 1 < bytes.size()) ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        encoded.push_back((i + 2 < bytes.size()) ? kAlphabet[triple & 0x3F] : '=');
    }

    return encoded;
}

// Authored animation keys (shared between the glTF fixture and assertions).
const std::vector<float> kAnimTimes = {0.0f, 1.0f};
const std::vector<float> kHeadRotationKeys = {
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.7071068f,
    0.7071068f,
};
const std::vector<float> kSpineTranslationKeys = {
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.25f,
    0.0f,
};

void WriteRiggedTriangleGltf(const std::filesystem::path& path) {
    const std::vector<float> positions = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
    };
    const std::vector<float> normals = {
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    const std::vector<float> uvs = {
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    const std::vector<uint8_t> jointIndices = {
        0,
        0,
        0,
        0,
        0,
        1,
        0,
        0,
        1,
        0,
        0,
        0,
    };
    const std::vector<float> jointWeights = {
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.5f,
        0.5f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    const std::vector<uint16_t> indices = {0, 1, 2};

    // Column-major inverse bind matrices: spine = identity, head undoes its
    // (0, 1, 0) bind translation.
    std::vector<float> inverseBind(32, 0.0f);
    inverseBind[0] = inverseBind[5] = inverseBind[10] = inverseBind[15] = 1.0f;
    inverseBind[16] = inverseBind[21] = inverseBind[26] = inverseBind[31] = 1.0f;
    inverseBind[29] = -1.0f; // head: translate(0, -1, 0)

    std::vector<unsigned char> buffer;
    const std::array<BufferView, 10> views = {
        AppendValues(buffer, positions),
        AppendValues(buffer, normals),
        AppendValues(buffer, uvs),
        AppendValues(buffer, jointIndices),
        AppendValues(buffer, jointWeights),
        AppendValues(buffer, indices),
        AppendValues(buffer, inverseBind),
        AppendValues(buffer, kAnimTimes),
        AppendValues(buffer, kHeadRotationKeys),
        AppendValues(buffer, kSpineTranslationKeys),
    };

    std::ostringstream gltf;
    gltf << R"({
  "asset": {"version": "2.0"},
  "buffers": [{
    "byteLength": )"
         << buffer.size() << R"(,
    "uri": "data:application/octet-stream;base64,)"
         << Base64Encode(buffer) << R"("
  }],
  "bufferViews": [
)";

    for (size_t i = 0; i < views.size(); ++i) {
        gltf << R"(    {"buffer": 0, "byteOffset": )" << views[i].offset << R"(, "byteLength": )"
             << views[i].length << "}";
        gltf << ((i + 1 == views.size()) ? "\n" : ",\n");
    }

    gltf << R"(  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4"},
    {"bufferView": 7, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0.0], "max": [1.0]},
    {"bufferView": 8, "componentType": 5126, "count": 2, "type": "VEC4"},
    {"bufferView": 9, "componentType": 5126, "count": 2, "type": "VEC3"}
  ],
  "meshes": [{
    "primitives": [
      {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 3, "WEIGHTS_0": 4}, "indices": 5}
    ]
  }],
  "skins": [{"joints": [0, 1], "inverseBindMatrices": 6, "skeleton": 0}],
  "animations": [{
    "name": "wiggle",
    "samplers": [
      {"input": 7, "output": 8, "interpolation": "LINEAR"},
      {"input": 7, "output": 9, "interpolation": "LINEAR"}
    ],
    "channels": [
      {"sampler": 0, "target": {"node": 1, "path": "rotation"}},
      {"sampler": 1, "target": {"node": 0, "path": "translation"}}
    ]
  }],
  "nodes": [
    {"name": "spine", "children": [1]},
    {"name": "head", "translation": [0.0, 1.0, 0.0]},
    {"name": "body", "mesh": 0, "skin": 0}
  ],
  "scenes": [{"nodes": [0, 2]}],
  "scene": 0
}
)";

    std::ofstream out(path);
    out << gltf.str();
}

std::vector<char> ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const SkinnedVertexData*
FindVertexByPosition(const SkinnedMeshAsset& mesh, float x, float y, float z) {
    for (const SkinnedVertexData& v : mesh.vertices) {
        if (std::fabs(v.pos[0] - x) < 0.0001f && std::fabs(v.pos[1] - y) < 0.0001f &&
            std::fabs(v.pos[2] - z) < 0.0001f) {
            return &v;
        }
    }
    return nullptr;
}

const AnimTrack*
FindTrack(const AnimClipAsset& clip, uint32_t nameHash, AnimTargetType targetType) {
    for (const AnimTrack& track : clip.tracks) {
        if (track.header.jointNameHash == nameHash &&
            track.header.targetType == static_cast<uint32_t>(targetType)) {
            return &track;
        }
    }
    return nullptr;
}

} // namespace

TEST(SkeletalAssetRoundTrip, RiggedGltfImportsToLms2AndLanimWithFieldEquality) {
    TempDirectory temp;
    const std::filesystem::path input = temp.path() / "rigged_triangle.gltf";
    const std::filesystem::path output = temp.path() / "rigged_triangle.lmesh";
    const std::filesystem::path animOutput = temp.path() / "rigged_triangle.wiggle.lanim";
    WriteRiggedTriangleGltf(input);

    process_gltf(input.string(), output.string());

    ASSERT_TRUE(std::filesystem::exists(output));
    ASSERT_TRUE(std::filesystem::exists(animOutput));

    // --- LMS2 mesh + skeleton ---
    SkinnedMeshAsset mesh;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    EXPECT_EQ(mesh.header.magic, kLms2Magic);
    EXPECT_EQ(mesh.header.version, 1u);
    EXPECT_EQ(mesh.header.vertexCount, 3u);
    EXPECT_EQ(mesh.header.indexCount, 3u);
    ASSERT_EQ(mesh.header.jointCount, 2u);
    EXPECT_NEAR(mesh.header.boundingSphere[0], 0.5f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[1], 0.5f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[2], 0.0f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[3], std::sqrt(0.5f), 0.0001f);

    const auto& spine = mesh.joints[0];
    EXPECT_EQ(spine.nameHash, HashJointName("spine"));
    EXPECT_EQ(spine.parentIndex, -1);
    EXPECT_FLOAT_EQ(spine.inverseBind[0], 1.0f);
    EXPECT_FLOAT_EQ(spine.inverseBind[13], 0.0f);
    EXPECT_FLOAT_EQ(spine.localTranslation[1], 0.0f);
    EXPECT_FLOAT_EQ(spine.localRotation[3], 1.0f);
    EXPECT_FLOAT_EQ(spine.localScale[0], 1.0f);

    const auto& head = mesh.joints[1];
    EXPECT_EQ(head.nameHash, HashJointName("head"));
    EXPECT_EQ(head.parentIndex, 0);
    EXPECT_FLOAT_EQ(head.inverseBind[13], -1.0f);
    EXPECT_FLOAT_EQ(head.localTranslation[0], 0.0f);
    EXPECT_FLOAT_EQ(head.localTranslation[1], 1.0f);
    EXPECT_FLOAT_EQ(head.localTranslation[2], 0.0f);

    // --- Vertices with skinning attributes preserved through the remap ---
    const SkinnedVertexData* v0 = FindVertexByPosition(mesh, 0.0f, 0.0f, 0.0f);
    const SkinnedVertexData* v1 = FindVertexByPosition(mesh, 1.0f, 0.0f, 0.0f);
    const SkinnedVertexData* v2 = FindVertexByPosition(mesh, 0.0f, 1.0f, 0.0f);
    ASSERT_NE(v0, nullptr);
    ASSERT_NE(v1, nullptr);
    ASSERT_NE(v2, nullptr);

    EXPECT_EQ(v0->joints[0], 0);
    EXPECT_EQ(v0->weights[0], 255);
    EXPECT_EQ(v0->weights[1], 0);
    EXPECT_EQ(static_cast<int>(v0->weights[0]) + v0->weights[1] + v0->weights[2] + v0->weights[3],
              255);

    EXPECT_EQ(v1->joints[0], 0);
    EXPECT_EQ(v1->joints[1], 1);
    // 0.5/0.5 quantizes deterministically (largest remainder, lower lane wins
    // ties) to 128/127, summing to exactly 255.
    EXPECT_EQ(v1->weights[0], 128);
    EXPECT_EQ(v1->weights[1], 127);
    EXPECT_EQ(static_cast<int>(v1->weights[0]) + v1->weights[1] + v1->weights[2] + v1->weights[3],
              255);

    EXPECT_EQ(v2->joints[0], 1);
    EXPECT_EQ(v2->weights[0], 255);

    for (uint32_t index : mesh.indices) {
        EXPECT_LT(index, mesh.header.vertexCount);
    }

    // ---.lanim sibling keyed by joint-name-hash ---
    AnimClipAsset clip;
    ASSERT_TRUE(LoadAnimClipAsset(animOutput.string(), clip));
    EXPECT_EQ(clip.header.magic, kLanimMagic);
    EXPECT_EQ(clip.header.version, 1u);
    EXPECT_EQ(clip.header.trackCount, 2u);
    EXPECT_FLOAT_EQ(clip.header.duration, 1.0f);

    const AnimTrack* rotation = FindTrack(clip, HashJointName("head"), AnimTargetType::Rotation);
    ASSERT_NE(rotation, nullptr);
    EXPECT_EQ(rotation->header.keyCount, 2u);
    EXPECT_EQ(rotation->header.componentCount, 4u);
    ASSERT_EQ(rotation->times.size(), kAnimTimes.size());
    ASSERT_EQ(rotation->values.size(), kHeadRotationKeys.size());
    EXPECT_EQ(
        std::memcmp(rotation->times.data(), kAnimTimes.data(), kAnimTimes.size() * sizeof(float)),
        0);
    EXPECT_EQ(std::memcmp(rotation->values.data(),
                          kHeadRotationKeys.data(),
                          kHeadRotationKeys.size() * sizeof(float)),
              0);

    const AnimTrack* translation =
        FindTrack(clip, HashJointName("spine"), AnimTargetType::Translation);
    ASSERT_NE(translation, nullptr);
    EXPECT_EQ(translation->header.keyCount, 2u);
    EXPECT_EQ(translation->header.componentCount, 3u);
    ASSERT_EQ(translation->values.size(), kSpineTranslationKeys.size());
    EXPECT_EQ(std::memcmp(
                  translation->times.data(), kAnimTimes.data(), kAnimTimes.size() * sizeof(float)),
              0);
    EXPECT_EQ(std::memcmp(translation->values.data(),
                          kSpineTranslationKeys.data(),
                          kSpineTranslationKeys.size() * sizeof(float)),
              0);
}

TEST(SkeletalAssetRoundTrip, ReimportIsBitwiseDeterministic) {
    TempDirectory temp;
    const std::filesystem::path input = temp.path() / "rigged_triangle.gltf";
    WriteRiggedTriangleGltf(input);

    const std::filesystem::path outputA = temp.path() / "run_a.lmesh";
    const std::filesystem::path outputB = temp.path() / "run_b.lmesh";
    process_gltf(input.string(), outputA.string());
    process_gltf(input.string(), outputB.string());

    const auto meshA = ReadAllBytes(outputA);
    const auto meshB = ReadAllBytes(outputB);
    ASSERT_FALSE(meshA.empty());
    EXPECT_EQ(meshA, meshB);

    const auto animA = ReadAllBytes(temp.path() / "run_a.wiggle.lanim");
    const auto animB = ReadAllBytes(temp.path() / "run_b.wiggle.lanim");
    ASSERT_FALSE(animA.empty());
    EXPECT_EQ(animA, animB);
}
