#include "luminumbra_common/animation/MorphMesh.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <type_traits>

bool process_morph_gltf_checked(const std::string&, const std::string&);
bool process_gltf_checked(const std::string&, const std::string&);
int asset_processor_main(int, char**);

namespace {
using namespace luminumbra::animation;
using Json = nlohmann::json;
namespace fs = std::filesystem;

void Put(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::vector<std::uint8_t> Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

struct Fixture {
    Json document{{"asset", {{"version", "2.0"}}},
                  {"scene", 0},
                  {"scenes", {{{"nodes", {0}}}}},
                  {"nodes", {{{"mesh", 0}}}},
                  {"buffers", {{{"byteLength", 0}}}},
                  {"bufferViews", Json::array()},
                  {"accessors", Json::array()}};
    std::vector<std::uint8_t> binary;

    template<typename T>
    std::size_t Accessor(const std::vector<T>& values,
                         const char* type,
                         std::size_t components,
                         std::uint32_t component_type = 5126) {
        static_assert(std::endian::native == std::endian::little);
        while (binary.size() % 4)
            binary.push_back(0);
        const auto offset = binary.size(), view = document["bufferViews"].size();
        binary.resize(offset + values.size() * sizeof(T));
        std::memcpy(binary.data() + offset, values.data(), values.size() * sizeof(T));
        document["bufferViews"].push_back(
            {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", values.size() * sizeof(T)}});
        const auto index = document["accessors"].size();
        document["accessors"].push_back({{"bufferView", view},
                                         {"componentType", component_type},
                                         {"type", type},
                                         {"count", values.size() / components}});
        return index;
    }
    Fixture() {
        Accessor<float>({0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0}, "VEC3", 3);
        Accessor<float>({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3", 3);
        Accessor<float>({0, 0, 1, 0, 0, 1, 0, 0}, "VEC2", 2);
        Accessor<float>({1, 0, 0, 1, 0, 0, 1, 0, 0, 3, 0, 0}, "VEC3", 3);
        Accessor<float>({0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}, "VEC3", 3);
        Accessor<float>({0, 2, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0}, "VEC3", 3);
        Accessor<std::uint16_t>({0, 1, 2, 3, 1, 2}, "SCALAR", 1, 5123);
        for (int index : {0, 3, 5}) {
            document["accessors"][index]["min"] = {0, 0, 0};
            document["accessors"][index]["max"] = {3, 2, 0};
        }
        document["meshes"] = {
            {{"primitives",
              {{{"mode", 4},
                {"indices", 6},
                {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                {"targets", {{{"POSITION", 3}, {"NORMAL", 4}}, {{"POSITION", 5}}}}}}},
             {"weights", {0.1, 0.2}}}};
        document["nodes"][0]["weights"] = {0.5, 0.25};
    }
    void Write(const fs::path& path) {
        while (binary.size() % 4)
            binary.push_back(0);
        document["buffers"][0]["byteLength"] = binary.size();
        std::string text = document.dump();
        while (text.size() % 4)
            text += ' ';
        std::vector<std::uint8_t> bytes(20);
        Put(bytes, 0, 0x46546c67);
        Put(bytes, 4, 2);
        Put(bytes, 8, static_cast<std::uint32_t>(28 + text.size() + binary.size()));
        Put(bytes, 12, static_cast<std::uint32_t>(text.size()));
        Put(bytes, 16, 0x4e4f534a);
        bytes.insert(bytes.end(), text.begin(), text.end());
        const auto offset = bytes.size();
        bytes.resize(offset + 8);
        Put(bytes, offset, static_cast<std::uint32_t>(binary.size()));
        Put(bytes, offset + 4, 0x004e4942);
        bytes.insert(bytes.end(), binary.begin(), binary.end());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
};

class MorphAssetTest : public ::testing::Test {
protected:
    fs::path directory;
    void SetUp() override {
        directory = fs::temp_directory_path() /
                    ("luminumbra_morph_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(fs::create_directory(directory));
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(directory, error);
    }
    bool Compile(Fixture& fixture) {
        fixture.Write(directory / "source.glb");
        return process_morph_gltf_checked((directory / "source.glb").string(),
                                          (directory / "mesh.lmorph").string());
    }
    std::shared_ptr<const MorphMeshAsset> Load() {
        std::shared_ptr<const MorphMeshAsset> result;
        EXPECT_TRUE(LoadMorphMeshAsset(directory / "mesh.lmorph", result));
        return result;
    }
    void Refuses(Fixture fixture) {
        const std::vector<std::uint8_t> sentinel{'o', 'l', 'd'};
        {
            std::ofstream file(directory / "mesh.lmorph", std::ios::binary);
            file << "old";
        }
        EXPECT_FALSE(Compile(fixture));
        EXPECT_EQ(Read(directory / "mesh.lmorph"), sentinel);
    }
};

TEST_F(MorphAssetTest, CompilesLoadsAndEvaluatesActualTwoTargetGeometry) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    const auto asset = Load();
    ASSERT_NE(asset, nullptr);
    const auto& data = asset->Data();
    EXPECT_EQ(data.indices, (std::vector<std::uint32_t>{0, 1, 2, 3, 1, 2}));
    EXPECT_EQ(data.default_weights, (std::vector<float>{0.5f, 0.25f}));
    ASSERT_EQ(data.vertices.size(), 4u);
    ASSERT_EQ(data.targets.size(), 2u);
    EXPECT_EQ(data.targets[1].vertices[0].normal, (std::array<float, 3>{0, 0, 0}));
    MorphFrame frame;
    ASSERT_TRUE(EvaluateMorphMesh(asset, data.default_weights, kMorphIdentity, frame));
    EXPECT_EQ(frame.vertices[0].position, (std::array<float, 3>{.5f, .5f, 0}));
    EXPECT_EQ(frame.vertices[3].position, (std::array<float, 3>{1.5f, .5f, 0}));
    EXPECT_NE(frame.vertices[0].position, frame.vertices[3].position); // Never base-only weld.
    EXPECT_NEAR(frame.vertices[0].normal[1], 1 / std::sqrt(5.0), 1e-6);
    EXPECT_NEAR(frame.vertices[0].normal[2], 2 / std::sqrt(5.0), 1e-6);
    EXPECT_EQ(frame.bounds_min, (std::array<float, 3>{.5f, .5f, 0}));
    EXPECT_EQ(frame.bounds_max, (std::array<float, 3>{1.5f, 1.5f, 0}));
}

TEST_F(MorphAssetTest, UsesNodeThenMeshThenZeroDefaults) {
    Fixture fixture;
    fixture.document["nodes"][0].erase("weights");
    ASSERT_TRUE(Compile(fixture));
    EXPECT_EQ(Load()->Data().default_weights, (std::vector<float>{.1f, .2f}));
    fixture.document["meshes"][0].erase("weights");
    ASSERT_TRUE(Compile(fixture));
    EXPECT_EQ(Load()->Data().default_weights, (std::vector<float>{0, 0}));
}

TEST_F(MorphAssetTest, PreservesExactEndpointsNegativeAndExtrapolatedWeights) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    for (const auto weights : {std::array<float, 2>{0, 0}, {1, 0}, {0, 1}, {-1, 2}}) {
        MorphFrame frame;
        ASSERT_TRUE(EvaluateMorphMesh(asset, weights, kMorphIdentity, frame));
        EXPECT_EQ(frame.vertices[0].position,
                  (std::array<float, 3>{weights[0], 2 * weights[1], 0}));
        EXPECT_EQ(frame.vertices[3].position[0], 3 * weights[0]);
    }
}

TEST_F(MorphAssetTest, AppliesDeltasBeforeParentShearMirrorAndCallerPlacement) {
    Fixture fixture;
    fixture.document["nodes"].push_back(
        {{"translation", {10, 2, 3}}, {"scale", {-2, 1, 1}}, {"children", {0}}});
    fixture.document["scenes"][0]["nodes"] = {1};
    fixture.document["nodes"][0]["rotation"] = {0, 0, std::sqrt(.2), std::sqrt(.8)};
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    auto placement = kMorphIdentity;
    placement[12] = 100;
    placement[13] = 200;
    placement[14] = 300;
    MorphFrame frame;
    ASSERT_TRUE(EvaluateMorphMesh(asset, asset->Data().default_weights, placement, frame));
    EXPECT_TRUE(frame.reverse_front_face);
    EXPECT_NEAR(frame.vertices[0].position[0], 110.2, 1e-5);
    EXPECT_NEAR(frame.vertices[0].position[1], 202.7, 1e-5);
    EXPECT_FLOAT_EQ(frame.vertices[0].position[2], 303);
    EXPECT_NEAR(frame.vertices[0].normal[0], .2 / std::sqrt(1.13), 1e-6);
    EXPECT_NEAR(frame.vertices[0].normal[1], .3 / std::sqrt(1.13), 1e-6);
    EXPECT_NEAR(frame.vertices[0].normal[2], 1 / std::sqrt(1.13), 1e-6);
    for (const auto& vertex : frame.vertices)
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_LE(frame.bounds_min[c], vertex.position[c]);
            EXPECT_GE(frame.bounds_max[c], vertex.position[c]);
        }
}

TEST_F(MorphAssetTest, PreservesNonindexedTopologyAndAbsentUvs) {
    Fixture fixture;
    auto& primitive = fixture.document["meshes"][0]["primitives"][0];
    primitive.erase("indices");
    primitive["attributes"].erase("TEXCOORD_0");
    for (int i = 0; i < 6; ++i)
        fixture.document["accessors"][i]["count"] = 3;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    EXPECT_EQ(asset->Data().indices, (std::vector<std::uint32_t>{0, 1, 2}));
    EXPECT_EQ(asset->Data().vertices[1].uv, (std::array<float, 2>{0, 0}));
}

TEST_F(MorphAssetTest, RepeatedCompileAndWireEncodingAreByteStable) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    const auto bytes = Read(directory / "mesh.lmorph");
    ASSERT_TRUE(Compile(fixture));
    EXPECT_EQ(Read(directory / "mesh.lmorph"), bytes);
    ASSERT_EQ(bytes.size(), 448u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "LMOR");
    auto asset = Load();
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(EncodeMorphMeshAsset(*asset, encoded));
    EXPECT_EQ(encoded, bytes);
}

TEST_F(MorphAssetTest, DecodeRefusesMalformedWireAndRetainsPreviousAsset) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    const auto previous = asset;
    const auto original = Read(directory / "mesh.lmorph");
    for (const auto& [offset, value] :
         std::vector<std::pair<std::size_t, std::uint32_t>>{{0, 0},
                                                            {4, 2},
                                                            {8, 1},
                                                            {12, UINT32_MAX},
                                                            {16, 2},
                                                            {20, 65},
                                                            {28, 1},
                                                            {32, 0},
                                                            {44, 0x3f800000},
                                                            {96, 0x7fc00000},
                                                            {104, 0x7f800000},
                                                            {116, 0x7fc00000},
                                                            {232, 4},
                                                            {256, 0x7fc00000}}) {
        auto bytes = original;
        Put(bytes, offset, value);
        EXPECT_FALSE(DecodeMorphMeshAsset(bytes, asset)) << offset;
        EXPECT_EQ(asset, previous);
    }
    for (const auto size : {0u, 95u, 447u, 449u}) {
        auto bytes = original;
        bytes.resize(size);
        EXPECT_FALSE(DecodeMorphMeshAsset(bytes, asset));
        EXPECT_EQ(asset, previous);
    }
}

TEST_F(MorphAssetTest, BuilderCannotPublishInvalidMutableTopology) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    const auto original = asset;
    for (int kind = 0; kind < 5; ++kind) {
        auto data = original->Data();
        if (kind == 0)
            data.targets[0].vertices.clear();
        if (kind == 1)
            data.default_weights.clear();
        if (kind == 2)
            data.indices[0] = 400;
        if (kind == 3)
            data.targets.resize(65);
        if (kind == 4)
            data.vertices[0].normal = {0, 0, 0};
        EXPECT_FALSE(CreateMorphMeshAsset(std::move(data), asset));
        EXPECT_EQ(asset, original);
    }
    static_assert(!std::is_copy_constructible_v<MorphMeshAsset>);
    static_assert(!std::is_move_constructible_v<MorphMeshAsset>);
}

TEST_F(MorphAssetTest, FailedEvaluationPreservesFrameAndOwnedAsset) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    MorphFrame frame;
    ASSERT_TRUE(EvaluateMorphMesh(asset, asset->Data().default_weights, kMorphIdentity, frame));
    const auto positions = frame.vertices[0].position;
    const auto minimum = frame.bounds_min;
    for (const auto& weights :
         {std::vector<float>{}, {0}, {0, 1, 2}, {std::numeric_limits<float>::infinity(), 0}}) {
        EXPECT_FALSE(EvaluateMorphMesh(asset, weights, kMorphIdentity, frame));
        EXPECT_EQ(frame.vertices[0].position, positions);
        EXPECT_EQ(frame.bounds_min, minimum);
    }
    auto singular = kMorphIdentity;
    singular[0] = 0;
    EXPECT_FALSE(EvaluateMorphMesh(asset, asset->Data().default_weights, singular, frame));
    singular = kMorphIdentity;
    singular[3] = 1e-7f;
    EXPECT_FALSE(EvaluateMorphMesh(asset, asset->Data().default_weights, singular, frame));
    EXPECT_FALSE(EvaluateMorphMesh({}, {}, kMorphIdentity, frame));
    EXPECT_EQ(frame.vertices[0].position, positions);
    EXPECT_EQ(frame.asset, asset);
    asset.reset();
    EXPECT_NE(frame.asset, nullptr);
}

TEST_F(MorphAssetTest, ZeroNormalsAndOverflowRefuseWithoutReplacingFrame) {
    Fixture fixture;
    ASSERT_TRUE(Compile(fixture));
    auto asset = Load();
    MorphFrame frame;
    ASSERT_TRUE(EvaluateMorphMesh(asset, asset->Data().default_weights, kMorphIdentity, frame));
    const auto previous = frame.asset;
    auto data = asset->Data();
    data.targets[0].vertices[0].normal = {0, 0, -1};
    ASSERT_TRUE(CreateMorphMeshAsset(data, asset));
    EXPECT_FALSE(EvaluateMorphMesh(asset, std::array<float, 2>{1, 0}, kMorphIdentity, frame));
    data.targets[0].vertices[0].normal = {0, 0, 0};
    data.targets[0].vertices[0].position[0] = std::numeric_limits<float>::max();
    ASSERT_TRUE(CreateMorphMeshAsset(data, asset));
    EXPECT_FALSE(EvaluateMorphMesh(asset, std::array<float, 2>{2, 0}, kMorphIdentity, frame));
    EXPECT_EQ(frame.asset, previous);
}

TEST_F(MorphAssetTest, DefaultMeshImporterStillRefusesMorphDataWithoutChangingOutput) {
    Fixture fixture;
    fixture.Write(directory / "source.glb");
    {
        std::ofstream file(directory / "mesh.lmesh");
        file << "old";
    }
    EXPECT_FALSE(process_gltf_checked((directory / "source.glb").string(),
                                      (directory / "mesh.lmesh").string()));
    EXPECT_EQ(Read(directory / "mesh.lmesh"), (std::vector<std::uint8_t>{'o', 'l', 'd'}));
}

TEST_F(MorphAssetTest, RefusesSkinsAnimationAndOptionalInstancing) {
    Fixture fixture;
    fixture.document["skins"] = {{{"joints", {0}}}};
    Refuses(fixture);
    fixture = Fixture{};
    const auto times = fixture.Accessor<float>({0, 1}, "SCALAR", 1);
    const auto values = fixture.Accessor<float>({0, 0, 1, 1}, "SCALAR", 1);
    fixture.document["animations"] = {
        {{"samplers", {{{"input", times}, {"output", values}}}},
         {"channels", {{{"sampler", 0}, {"target", {{"node", 0}, {"path", "weights"}}}}}}}};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["nodes"][0]["extensions"] = {
        {"EXT_mesh_gpu_instancing", {{"attributes", {{"TRANSLATION", 0}}}}}};
    Refuses(fixture); // Deliberately absent from extensionsRequired/Used.
}

TEST_F(MorphAssetTest, RefusesSparseAndUnknownTargetOrBaseAttributes) {
    Fixture fixture;
    fixture.document["accessors"][3]["sparse"] = {
        {"count", 1},
        {"indices", {{"bufferView", 6}, {"componentType", 5123}}},
        {"values", {{"bufferView", 3}}}};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["meshes"][0]["primitives"][0]["targets"][0]["TANGENT"] = 3;
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["meshes"][0]["primitives"][0]["attributes"]["_CUSTOM"] = 3;
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["accessors"][3]["count"] = 3;
    Refuses(fixture);
}

TEST_F(MorphAssetTest, RefusesAmbiguousInstancesScenesTopologyAndDefaultWeights) {
    Fixture fixture;
    fixture.document["nodes"].push_back({{"mesh", 0}});
    fixture.document["scenes"][0]["nodes"] = {0, 1};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document.erase("scene");
    fixture.document["scenes"].push_back({{"nodes", {0}}});
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["meshes"][0]["primitives"][0]["mode"] = 1;
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["nodes"][0]["weights"] = {1};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["meshes"][0]["primitives"].push_back(
        fixture.document["meshes"][0]["primitives"][0]);
    Refuses(fixture);
}

TEST_F(MorphAssetTest, RefusesExternalDataExtensionsAndSingularSourceTransforms) {
    Fixture fixture;
    fixture.document["buffers"][0]["uri"] = "external.bin";
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["extensionsUsed"] = {"UNKNOWN_optional"};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["nodes"][0]["scale"] = {0, 1, 1};
    Refuses(fixture);
    fixture = Fixture{};
    fixture.document["nodes"][0]["matrix"] = kMorphIdentity;
    fixture.document["nodes"][0]["matrix"][3] = 1e-7;
    Refuses(fixture);
}

TEST_F(MorphAssetTest, ActualCommandLineSelectsOnlyExplicitMorphProfile) {
    Fixture fixture; fixture.Write(directory / "source.glb");
    std::vector<std::string> values{"asset_processor", (directory / "source.glb").string(),
                                    (directory / "command.lmorph").string()};
    std::vector<char*> argv;
    for (auto& value : values) argv.push_back(value.data());
    ASSERT_EQ(asset_processor_main(static_cast<int>(argv.size()), argv.data()), 0);
    const auto before = Read(directory / "command.lmorph");
    std::string option = "--emit-lods"; argv.push_back(option.data());
    EXPECT_NE(asset_processor_main(static_cast<int>(argv.size()), argv.data()), 0);
    EXPECT_EQ(Read(directory / "command.lmorph"), before);
    EXPECT_FALSE(fs::exists(directory / "command.lod1.lmesh"));
}

TEST_F(MorphAssetTest, RefusesOverflowingMisalignedSourceExtentsBeforeReadback) {
    for (const char* field : {"byteOffset", "byteStride", "byteLength"}) {
        Fixture fixture; fixture.document["bufferViews"][0][field] = UINT64_MAX; Refuses(fixture);
    }
    Fixture fixture; fixture.document["accessors"][6]["byteOffset"] = UINT64_MAX; Refuses(fixture);
    fixture = Fixture{}; fixture.document["accessors"][6]["count"] = UINT64_MAX; Refuses(fixture);
    fixture = Fixture{}; fixture.document["accessors"][0]["byteOffset"] = 1; Refuses(fixture);
    fixture = Fixture{}; fixture.document["bufferViews"][0]["byteOffset"] = 1; Refuses(fixture);
}
} // namespace
