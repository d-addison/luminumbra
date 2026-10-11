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
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/animation/SkinnedMeshFormat.h"

void process_gltf(const std::string& input_path, const std::string& output_path);
bool process_gltf_checked(const std::string& input_path, const std::string& output_path);

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

void WriteRiggedTriangleGltf(const std::filesystem::path& path,
                             bool childFirst = false,
                             const std::string& interpolation = "LINEAR") {
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
    std::vector<uint8_t> jointIndices = {
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
    if (childFirst) {
        for (auto& index : jointIndices)
            index = 1 - index;
        std::swap_ranges(inverseBind.begin(), inverseBind.begin() + 16, inverseBind.begin() + 16);
    }

    auto times = kAnimTimes;
    auto rotationKeys = kHeadRotationKeys;
    auto translationKeys = kSpineTranslationKeys;
    if (interpolation == "CUBICSPLINE") {
        times = {0, 2};
        rotationKeys = {0, 0, 0, 0, 0, 0, 0,          1,          0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0.7071068f, 0.7071068f, 0, 0, 0, 0};
        translationKeys = {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0};
    }
    std::vector<unsigned char> buffer;
    const std::array<BufferView, 10> views = {
        AppendValues(buffer, positions),
        AppendValues(buffer, normals),
        AppendValues(buffer, uvs),
        AppendValues(buffer, jointIndices),
        AppendValues(buffer, jointWeights),
        AppendValues(buffer, indices),
        AppendValues(buffer, inverseBind),
        AppendValues(buffer, times),
        AppendValues(buffer, rotationKeys),
        AppendValues(buffer, translationKeys),
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
    auto json = gltf.str();
    if (childFirst)
        json.replace(json.find("\"joints\": [0, 1]"), 16, "\"joints\": [1, 0]");
    for (size_t at = 0; (at = json.find("LINEAR", at)) != std::string::npos;) {
        json.replace(at, 6, interpolation);
        at += interpolation.size();
    }
    if (interpolation == "CUBICSPLINE") {
        const auto timeMax = json.find("\"max\": [1.0]", json.find("\"bufferView\": 7"));
        json.replace(timeMax, 12, "\"max\": [2.0]");
        for (const auto view : {8, 9}) {
            const auto at =
                json.find("\"count\": 2", json.find("\"bufferView\": " + std::to_string(view)));
            json.replace(at, 10, "\"count\": 6");
        }
    }
    out << json;
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
    EXPECT_EQ(clip.header.version, 2u);
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

namespace {

bool ChangeRig(const std::filesystem::path& path, const std::string& from, const std::string& to) {
    auto bytes = ReadAllBytes(path);
    std::string text(bytes.begin(), bytes.end());
    const auto at = text.find(from);
    if (at == std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    std::ofstream output(path);
    output << text;
    return static_cast<bool>(output);
}

std::vector<float> BindPalette(const SkinnedMeshAsset& mesh) {
    const auto skeleton = luminumbra::animation::BuildSkeleton(mesh);
    std::vector<float> palette;
    luminumbra::animation::ComputeJointPalette(
        skeleton, luminumbra::animation::MakeBindPose(skeleton), palette);
    return palette;
}

} // namespace

TEST(SkeletalSceneImport, NormalizesJointOrderAndRemapsInfluencesAndInverseBinds) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    const auto expected = ReadAllBytes(output);
    WriteRiggedTriangleGltf(source, true);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    EXPECT_EQ(ReadAllBytes(output), expected);
    SkinnedMeshAsset mesh;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    const auto palette = BindPalette(mesh);
    for (size_t i = 0; i < palette.size(); ++i)
        EXPECT_NEAR(palette[i], (i % 16) % 5 == 0 ? 1.0f : 0.0f, 1e-6f);
}

TEST(SkeletalSceneImport, MatrixBindMatchesEquivalentTrs) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    const auto expected = ReadAllBytes(output);
    ASSERT_TRUE(ChangeRig(source,
                          R"("translation": [0.0, 1.0, 0.0])",
                          R"("matrix": [1,0,0,0,0,1,0,0,0,0,1,0,0,1,0,1])"));
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    EXPECT_EQ(ReadAllBytes(output), expected);
}

TEST(SkeletalSceneImport, RetainsNonJointAncestorsAndTheirAnimation) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(ChangeRig(
        source,
        R"({"name": "body", "mesh": 0, "skin": 0})",
        R"({"name": "body", "mesh": 0, "skin": 0}, {"name": "armature", "translation": [2,0,0], "children": [0]})"));
    ASSERT_TRUE(ChangeRig(source, R"("nodes": [0, 2])", R"("nodes": [3, 2])"));
    ASSERT_TRUE(ChangeRig(
        source, R"("node": 0, "path": "translation")", R"("node": 3, "path": "translation")"));
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    SkinnedMeshAsset mesh;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    ASSERT_EQ(mesh.joints.size(), 3u);
    EXPECT_EQ(mesh.joints[0].nameHash, HashJointName("armature"));
    EXPECT_EQ(mesh.joints[1].parentIndex, 0);
    EXPECT_EQ(mesh.joints[2].parentIndex, 1);
    const auto palette = BindPalette(mesh);
    EXPECT_FLOAT_EQ(palette[16 + 12], 2.0f);
    EXPECT_FLOAT_EQ(palette[32 + 12], 2.0f);
    AnimClipAsset clip;
    ASSERT_TRUE(LoadAnimClipAsset((temp.path() / "rig.wiggle.lanim").string(), clip));
    const auto skeleton = luminumbra::animation::BuildSkeleton(mesh);
    const auto pose =
        luminumbra::animation::SamplePose(skeleton, luminumbra::animation::BuildClip(clip), 0.5f);
    EXPECT_FLOAT_EQ(pose.joints[0].translation[1], 0.125f);
}

TEST(SkeletalSceneImport, UsesSkinReferencedBySceneMesh) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    const auto expected = ReadAllBytes(output);
    ASSERT_TRUE(ChangeRig(source, R"("skins": [)", R"("skins": [{"joints": [1]}, )"));
    ASSERT_TRUE(ChangeRig(source, R"("skin": 0)", R"("skin": 1)"));
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    EXPECT_EQ(ReadAllBytes(output), expected);
}

TEST(SkeletalSceneImport, InvalidRigPreservesExistingMesh) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    const std::vector<std::pair<std::string, std::string>> changes = {
        {R"("name": "head")", R"("name": "spine")"},
        {R"("JOINTS_0": 3)", R"("JOINTS_0": 3, "JOINTS_1": 3, "WEIGHTS_1": 4)"},
        {R"("translation": [0.0, 1.0, 0.0])", R"("matrix": [1,0,0,0,1,1,0,0,0,0,1,0,0,1,0,1])"},
        {R"("NORMAL": 1, )", ""},
        {R"("count": 2, "type": "MAT4")", R"("count": 1, "type": "MAT4")"},
        {R"("joints": [0, 1])", R"("joints": [0, 0])"},
    };
    for (const auto& [from, to] : changes) {
        SCOPED_TRACE(to);
        WriteRiggedTriangleGltf(source);
        ASSERT_TRUE(ChangeRig(source, from, to));
        {
            std::ofstream previous(output);
            previous << "previous-mesh";
        }
        EXPECT_FALSE(process_gltf_checked(source.string(), output.string()));
        const auto bytes = ReadAllBytes(output);
        EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "previous-mesh");
    }
}

TEST(AnimationImport, StepHoldsUntilExactNextKey) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source, false, "STEP");
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    SkinnedMeshAsset mesh;
    AnimClipAsset clip;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    ASSERT_TRUE(LoadAnimClipAsset((temp.path() / "rig.wiggle.lanim").string(), clip));
    ASSERT_EQ(clip.header.version, 2u);
    ASSERT_FALSE(clip.tracks.empty());
    for (const auto& track : clip.tracks)
        EXPECT_EQ(track.interpolation, luminumbra::animation::AnimInterpolation::Step);
    const auto skeleton = luminumbra::animation::BuildSkeleton(mesh);
    const auto runtime = luminumbra::animation::BuildClip(clip);
    EXPECT_FLOAT_EQ(
        luminumbra::animation::SamplePose(skeleton, runtime, 0.75f).joints[0].translation[1], 0);
    EXPECT_FLOAT_EQ(
        luminumbra::animation::SamplePose(skeleton, runtime, 1.0f).joints[0].translation[1], 0.25f);
}

TEST(AnimationImport, CubicSplineUsesValuesTangentsAndIntervalLength) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source, false, "CUBICSPLINE");
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    SkinnedMeshAsset mesh;
    AnimClipAsset clip;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    ASSERT_TRUE(LoadAnimClipAsset((temp.path() / "rig.wiggle.lanim").string(), clip));
    ASSERT_EQ(clip.header.version, 2u);
    ASSERT_FALSE(clip.tracks.empty());
    for (const auto& track : clip.tracks)
        EXPECT_EQ(track.interpolation, luminumbra::animation::AnimInterpolation::CubicSpline);
    const auto skeleton = luminumbra::animation::BuildSkeleton(mesh);
    const auto runtime = luminumbra::animation::BuildClip(clip);
    const auto middle = luminumbra::animation::SamplePose(skeleton, runtime, 1.0f);
    EXPECT_FLOAT_EQ(middle.joints[0].translation[1], 0.375f);
    EXPECT_NEAR(middle.joints[1].rotation[2], 0.38268343f, 1e-6f);
    EXPECT_NEAR(middle.joints[1].rotation[3], 0.92387953f, 1e-6f);
    EXPECT_FLOAT_EQ(luminumbra::animation::SamplePose(skeleton, runtime, 0).joints[1].rotation[3],
                    1);
    EXPECT_FLOAT_EQ(
        luminumbra::animation::SamplePose(skeleton, runtime, 2).joints[0].translation[1], 0.25f);
}

TEST(AnimationImport, LinearRotationFollowsSphericalInterpolation) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    SkinnedMeshAsset mesh;
    AnimClipAsset clip;
    ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
    ASSERT_TRUE(LoadAnimClipAsset((temp.path() / "rig.wiggle.lanim").string(), clip));
    ASSERT_EQ(clip.header.version, 2u);
    ASSERT_FALSE(clip.tracks.empty());
    for (const auto& track : clip.tracks)
        EXPECT_EQ(track.interpolation, luminumbra::animation::AnimInterpolation::Linear);
    const auto pose = luminumbra::animation::SamplePose(
        luminumbra::animation::BuildSkeleton(mesh), luminumbra::animation::BuildClip(clip), 0.25f);
    EXPECT_NEAR(pose.joints[1].rotation[2], 0.19509032f, 1e-4f);
    EXPECT_NEAR(pose.joints[1].rotation[3], 0.98078528f, 1e-4f);
}

TEST(AnimationImport, InvalidClipPreservesExistingMeshAndClips) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    const auto animation = temp.path() / "rig.wiggle.lanim";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    const auto originalMesh = ReadAllBytes(output);
    const auto originalClip = ReadAllBytes(animation);
    const std::vector<std::pair<std::string, std::string>> changes = {
        {R"("interpolation": "LINEAR")", R"("interpolation": "UNKNOWN")"},
        {R"("name": "wiggle")", R"("name": "../escape")"},
        {R"("node": 0, "path": "translation")", R"("node": 2, "path": "translation")"},
        {R"("input": 7, "output": 8)", R"("input": 7, "output": 9)"},
    };
    for (const auto& [from, to] : changes) {
        SCOPED_TRACE(to);
        WriteRiggedTriangleGltf(source);
        ASSERT_TRUE(ChangeRig(source, from, to));
        EXPECT_FALSE(process_gltf_checked(source.string(), output.string()));
        EXPECT_EQ(ReadAllBytes(output), originalMesh);
        EXPECT_EQ(ReadAllBytes(animation), originalClip);
    }
}

TEST(SkeletalSceneImport, LoaderRejectsMalformedMeshWithoutReplacingAsset) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
    const auto valid = ReadAllBytes(output);
    const std::vector<std::pair<size_t, uint32_t>> changes = {
        {8, UINT32_MAX},
        {16, 300},
        {36 + 32, 255},
        {36 + 36, 0},
        {36 + 120, 99},
        {36 + 120 + 12 + 4, 1},
        {36 + 120 + 12 + 8 + 12, 1},
    };
    for (const auto& [offset, value] : changes) {
        SCOPED_TRACE(offset);
        auto bytes = valid;
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
        {
            std::ofstream file(output, std::ios::binary);
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        SkinnedMeshAsset asset;
        asset.header.vertexCount = 42;
        EXPECT_FALSE(LoadSkinnedMeshAsset(output.string(), asset));
        EXPECT_EQ(asset.header.vertexCount, 42u);
    }
}

TEST(SkeletalSceneImport, DecomposesRotatedScaledAndMirroredMatrixBinds) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    for (const int scale : {2, -2}) {
        SCOPED_TRACE(scale);
        WriteRiggedTriangleGltf(source);
        ASSERT_TRUE(ChangeRig(source,
                              R"({"name": "spine", "children": [1]})",
                              R"({"name": "spine", "children": [1], "matrix": [0,)" +
                                  std::to_string(scale) + R"(,0,0,-3,0,0,0,0,0,4,0,5,6,7,1]})"));
        ASSERT_TRUE(process_gltf_checked(source.string(), output.string()));
        SkinnedMeshAsset mesh;
        ASSERT_TRUE(LoadSkinnedMeshAsset(output.string(), mesh));
        const auto palette = BindPalette(mesh);
        const float expected[16] = {
            0, static_cast<float>(scale), 0, 0, -3, 0, 0, 0, 0, 0, 4, 0, 5, 6, 7, 1};
        for (size_t i = 0; i < palette.size(); ++i)
            EXPECT_NEAR(palette[i], expected[i % 16], 2e-6f);
        EXPECT_NEAR(mesh.header.boundingSphere[0], 3.5f, 2e-6f);
        EXPECT_NEAR(mesh.header.boundingSphere[1], 6 + scale / 2.0f, 2e-6f);
    }
}

TEST(SkeletalSceneImport, RefusesMultipleActiveSkinsBeforeOutput) {
    const TempDirectory temp;
    const auto source = temp.path() / "rig.gltf";
    const auto output = temp.path() / "rig.lmesh";
    WriteRiggedTriangleGltf(source);
    ASSERT_TRUE(ChangeRig(source, R"("skins": [)", R"("skins": [{"joints": [0,1]}, )"));
    ASSERT_TRUE(ChangeRig(
        source,
        R"({"name": "body", "mesh": 0, "skin": 0})",
        R"({"name": "body", "mesh": 0, "skin": 0}, {"name": "second", "mesh": 0, "skin": 1})"));
    ASSERT_TRUE(ChangeRig(source, R"("nodes": [0, 2])", R"("nodes": [0,2,3])"));
    EXPECT_FALSE(process_gltf_checked(source.string(), output.string()));
    EXPECT_FALSE(std::filesystem::exists(output));
}
