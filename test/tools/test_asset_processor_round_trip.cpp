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
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// stb_image declarations only; the implementation is compiled into
// asset_processor.cpp which is linked into this test executable.
#include "stb_image.h"
#include "stb_image_write.h"

void process_gltf(const std::string& input_path, const std::string& output_path);
bool process_texture(const std::string& input_path, const std::string& output_path);
bool process_gltf_checked(const std::string& input_path, const std::string& output_path);
int asset_processor_main(int argc, char* argv[]);

namespace {

constexpr uint32_t kLmeshMagic = 0x48534D4C;

struct LMeshHeader {
    uint32_t magic;
    uint32_t vertexCount;
    uint32_t indexCount;
    float boundingSphere[4];
};

struct Vertex {
    float pos[3];
    float norm[3];
    float uv[2];
};

struct BufferView {
    size_t offset;
    size_t length;
};

struct ExpectedVertex {
    std::array<float, 3> pos;
    std::array<float, 3> norm;
    std::array<float, 2> uv;
};

struct ProcessedMesh {
    LMeshHeader header;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("luminumbra_asset_round_trip_" + std::to_string(stamp));
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

int Import(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "asset_processor");
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return asset_processor_main(static_cast<int>(argv.size()), argv.data());
}

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

std::vector<ExpectedVertex> WriteTwoPrimitiveGltf(const std::filesystem::path& path,
                                                  float firstX = 0.0f) {
    const std::vector<float> positions0 = {
        firstX,
        0.0f,
        0.0f,
        2.0f,
        0.0f,
        0.0f,
        0.0f,
        2.0f,
        0.0f,
    };
    const std::vector<float> positions1 = {
        4.0f,
        0.0f,
        0.0f,
        4.0f,
        2.0f,
        0.0f,
        2.0f,
        2.0f,
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
    const std::vector<float> uvs0 = {
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    const std::vector<float> uvs1 = {
        1.0f,
        0.0f,
        1.0f,
        1.0f,
        0.0f,
        1.0f,
    };
    const std::vector<uint16_t> indices = {0, 1, 2};

    std::vector<unsigned char> buffer;
    const std::array<BufferView, 8> views = {
        AppendValues(buffer, positions0),
        AppendValues(buffer, normals),
        AppendValues(buffer, uvs0),
        AppendValues(buffer, indices),
        AppendValues(buffer, positions1),
        AppendValues(buffer, normals),
        AppendValues(buffer, uvs1),
        AppendValues(buffer, indices),
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
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [2, 2, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3", "min": [2, 0, 0], "max": [4, 2, 0]},
    {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 6, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 7, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "meshes": [{
    "primitives": [
      {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3},
      {"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6}, "indices": 7}
    ]
  }],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
}
)";

    std::ofstream out(path);
    out << gltf.str();

    return {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{0.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{4.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{4.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{2.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
}

ProcessedMesh ReadLmesh(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    ProcessedMesh mesh{};

    in.read(reinterpret_cast<char*>(&mesh.header), sizeof(mesh.header));
    mesh.vertices.resize(mesh.header.vertexCount);
    mesh.indices.resize(mesh.header.indexCount);
    in.read(reinterpret_cast<char*>(mesh.vertices.data()), mesh.vertices.size() * sizeof(Vertex));
    in.read(reinterpret_cast<char*>(mesh.indices.data()), mesh.indices.size() * sizeof(uint32_t));

    return mesh;
}

bool NearlyEqual(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.0001f;
}

bool MatchesVertex(const Vertex& actual, const ExpectedVertex& expected) {
    return NearlyEqual(actual.pos[0], expected.pos[0]) &&
           NearlyEqual(actual.pos[1], expected.pos[1]) &&
           NearlyEqual(actual.pos[2], expected.pos[2]) &&
           NearlyEqual(actual.norm[0], expected.norm[0]) &&
           NearlyEqual(actual.norm[1], expected.norm[1]) &&
           NearlyEqual(actual.norm[2], expected.norm[2]) &&
           NearlyEqual(actual.uv[0], expected.uv[0]) && NearlyEqual(actual.uv[1], expected.uv[1]);
}

} // namespace

TEST(AssetProcessorRoundTrip, WritesCombinedLmeshThatCanBeReadBack) {
    static_assert(sizeof(LMeshHeader) == 28);
    static_assert(sizeof(Vertex) == 32);

    TempDirectory temp;
    const std::filesystem::path input = temp.path() / "two_primitives.gltf";
    const std::filesystem::path output = temp.path() / "two_primitives.lmesh";
    const std::vector<ExpectedVertex> expectedVertices = WriteTwoPrimitiveGltf(input);

    process_gltf(input.string(), output.string());

    ASSERT_TRUE(std::filesystem::exists(output));
    const ProcessedMesh mesh = ReadLmesh(output);

    EXPECT_EQ(mesh.header.magic, kLmeshMagic);
    EXPECT_EQ(mesh.header.vertexCount, expectedVertices.size());
    EXPECT_EQ(mesh.header.indexCount, 6u);
    EXPECT_NEAR(mesh.header.boundingSphere[0], 2.0f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[1], 1.0f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[2], 0.0f, 0.0001f);
    EXPECT_NEAR(mesh.header.boundingSphere[3], std::sqrt(5.0f), 0.0001f);

    ASSERT_EQ(mesh.vertices.size(), expectedVertices.size());
    ASSERT_EQ(mesh.indices.size(), 6u);

    for (const ExpectedVertex& expected : expectedVertices) {
        const auto found =
            std::find_if(mesh.vertices.begin(), mesh.vertices.end(), [&](const Vertex& actual) {
                return MatchesVertex(actual, expected);
            });

        EXPECT_NE(found, mesh.vertices.end());
    }

    std::vector<bool> referenced(mesh.vertices.size(), false);
    for (uint32_t index : mesh.indices) {
        ASSERT_LT(index, mesh.vertices.size());
        referenced[index] = true;
    }

    EXPECT_TRUE(
        std::all_of(referenced.begin(), referenced.end(), [](bool value) { return value; }));
}

TEST(AssetProcessorRoundTrip, AppliesMaterialTextureTransformAndItsUvSetOverride) {
    TempDirectory temp;
    const auto input = temp.path() / "transformed.gltf";
    const auto output = temp.path() / "transformed.lmesh";
    auto expected = WriteTwoPrimitiveGltf(input);
    std::ifstream source(input);
    std::string json((std::istreambuf_iterator<char>(source)), {});
    source.close();
    const std::string attribute = "\"TEXCOORD_0\": 2}, \"indices\": 3";
    const auto location = json.find(attribute);
    ASSERT_NE(location, std::string::npos);
    json.replace(location,
                 attribute.size(),
                 "\"TEXCOORD_0\": 2, \"TEXCOORD_1\": 6}, \"indices\": 3, \"material\": 0");
    json.insert(1, R"(
      "extensionsUsed": ["KHR_texture_transform"],
      "images": [{"uri": "unused.png"}],
      "textures": [{"source": 0}],
      "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {
        "index": 0, "texCoord": 0, "extensions": {"KHR_texture_transform": {
          "texCoord": 1, "offset": [0.25, 0.5], "scale": [2, 3], "rotation": 1.5707963267948966
        }}
      }}}],
    )");
    std::ofstream modified(input);
    modified << json;
    modified.close();
    expected[0].uv = {0.25f, 2.5f};
    expected[1].uv = {-2.75f, 2.5f};
    expected[2].uv = {-2.75f, 0.5f};
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    const auto mesh = ReadLmesh(output);
    ASSERT_EQ(mesh.vertices.size(), expected.size());
    for (const auto& vertex : expected)
        EXPECT_TRUE(
            std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [&](const Vertex& actual) {
                return MatchesVertex(actual, vertex);
            }));
}

TEST(AssetProcessorRoundTrip, CommandReportsMissingInputAndLodWriteFailure) {
    TempDirectory temp;
    const auto input = temp.path() / "source.gltf";
    const auto output = temp.path() / "mesh.lmesh";
    EXPECT_NE(Import({input.string(), output.string()}), 0);
    EXPECT_FALSE(std::filesystem::exists(output));
    WriteTwoPrimitiveGltf(input);
    std::filesystem::create_directory(temp.path() / "mesh.lod1.lmesh");
    EXPECT_NE(Import({input.string(), output.string(), "--emit-lods"}), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(output));
}

namespace {

bool ReplaceGltf(const std::filesystem::path& path,
                 const std::string& from,
                 const std::string& to) {
    std::ifstream input(path);
    std::string json((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const auto position = json.find(from);
    if (position == std::string::npos)
        return false;
    json.replace(position, from.size(), to);
    std::ofstream output(path);
    output << json;
    return static_cast<bool>(output);
}

std::string FileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

void ExpectVertices(const ProcessedMesh& mesh, const std::vector<ExpectedVertex>& expected) {
    ASSERT_EQ(mesh.vertices.size(), expected.size());
    for (const auto& vertex : expected) {
        EXPECT_TRUE(
            std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [&](const Vertex& actual) {
                return MatchesVertex(actual, vertex);
            }));
    }
}

} // namespace

TEST(StaticSceneImport, PreservesNestedTransformsAndRepeatedInstances) {
    const TempDirectory temp;
    const auto input = temp.path() / "scene.gltf";
    const auto output = temp.path() / "scene.lmesh";
    const auto authored = WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(input, R"("nodes": [{"mesh": 0}])", R"("nodes": [
      {"translation": [3,1,-2], "rotation": [0,0,0.7071067811865476,0.7071067811865476],
       "scale": [2,3,1], "children": [1,2]},
      {"mesh": 0}, {"mesh": 0, "translation": [-1,0,2], "scale": [-1,1,1]}])"));
    std::vector<ExpectedVertex> expected;
    for (auto v : authored) {
        const auto p = v.pos;
        v.pos = {3 - 3 * p[1], 1 + 2 * p[0], -2};
        expected.push_back(v);
        v.pos = {3 - 3 * p[1], -1 - 2 * p[0], 0};
        expected.push_back(v);
    }
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    const auto mesh = ReadLmesh(output);
    EXPECT_EQ(mesh.indices.size(), 12u);
    ExpectVertices(mesh, expected);
    const auto first = FileBytes(output);
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    EXPECT_EQ(first, FileBytes(output));
}

TEST(StaticSceneImport, UsesSelectedSceneAndExcludesOtherInstances) {
    const TempDirectory temp;
    const auto input = temp.path() / "scene.gltf";
    const auto output = temp.path() / "scene.lmesh";
    auto expected = WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(
        input,
        R"("nodes": [{"mesh": 0}])",
        R"("nodes": [{"mesh": 0, "translation": [10,0,0]}, {"mesh": 0, "translation": [100,0,0]}])"));
    ASSERT_TRUE(ReplaceGltf(
        input, R"("scenes": [{"nodes": [0]}])", R"("scenes": [{"nodes": [0]}, {"nodes": [1]}])"));
    ASSERT_TRUE(ReplaceGltf(input, R"("scene": 0)", R"("scene": 1)"));
    for (auto& v : expected)
        v.pos[0] += 100;
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    ExpectVertices(ReadLmesh(output), expected);
}

TEST(StaticSceneImport, BakesShearWithInverseTransposeNormals) {
    const TempDirectory temp;
    const auto input = temp.path() / "shear.gltf";
    const auto output = temp.path() / "shear.lmesh";
    auto expected = WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(
        ReplaceGltf(input,
                    R"("nodes": [{"mesh": 0}])",
                    R"("nodes": [{"mesh": 0, "matrix": [2,0,1,0, 0,3,0,0, 0,0,4,0, 5,-1,2,1]}])"));
    for (auto& v : expected) {
        const auto p = v.pos;
        v.pos = {2 * p[0] + 5, 3 * p[1] - 1, p[0] + 2};
        v.norm = {-1 / std::sqrt(5.0f), 0, 2 / std::sqrt(5.0f)};
    }
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    ExpectVertices(ReadLmesh(output), expected);
}

TEST(StaticSceneImport, ReversesMirroredTriangleWinding) {
    const TempDirectory temp;
    const auto input = temp.path() / "mirror.gltf";
    const auto output = temp.path() / "mirror.lmesh";
    auto expected = WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(
        input, R"("nodes": [{"mesh": 0}])", R"("nodes": [{"mesh": 0, "scale": [-2,3,1]}])"));
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    const auto mesh = ReadLmesh(output);
    for (auto& v : expected) {
        v.pos[0] *= -2;
        v.pos[1] *= 3;
    }
    ExpectVertices(mesh, expected);
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i + 1]];
        const auto& c = mesh.vertices[mesh.indices[i + 2]];
        const float crossZ = (b.pos[0] - a.pos[0]) * (c.pos[1] - a.pos[1]) -
                             (b.pos[1] - a.pos[1]) * (c.pos[0] - a.pos[0]);
        EXPECT_GT(crossZ * a.norm[2], 0);
    }
}

TEST(StaticSceneImport, PrimitiveSelectionKeepsEverySelectedInstance) {
    const TempDirectory temp;
    const auto input = temp.path() / "parts.gltf";
    const auto output = temp.path() / "parts.lmesh";
    WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(input,
                            R"("nodes": [{"mesh": 0}])",
                            R"("nodes": [{"mesh": 0}, {"mesh": 0, "translation": [0,0,2]}])"));
    ASSERT_TRUE(
        ReplaceGltf(input, R"("scenes": [{"nodes": [0]}])", R"("scenes": [{"nodes": [0,1]}])"));
    ASSERT_EQ(Import({input.string(), output.string(), "--primitive", "1"}), 0);
    const auto mesh = ReadLmesh(output);
    EXPECT_EQ(mesh.indices.size(), 6u);
    for (const auto& v : mesh.vertices)
        EXPECT_GE(v.pos[0], 2.0f);
    // Reset the CLI's per-part selection before subsequent public function calls.
    ASSERT_EQ(Import({input.string(), output.string()}), 0);
}

TEST(StaticSceneImport, InvalidSelectedGeometryRetainsPreviousOutput) {
    const TempDirectory temp;
    const auto input = temp.path() / "invalid.gltf";
    const auto output = temp.path() / "valid.lmesh";
    const std::vector<std::pair<std::string, std::string>> mutations = {
        {R"("nodes": [{"mesh": 0}])", R"("nodes": [{"mesh": 0, "scale": [0,1,1]}])"},
        {R"("POSITION": 0, "NORMAL": 1,)", R"("POSITION": 0,)"},
        {R"("indices": 3})", R"("indices": 3, "mode": 1})"},
        {R"("asset": {"version": "2.0"})",
         R"("asset": {"version": "2.0"}, "extensionsRequired": ["UNKNOWN_required"])"},
        {R"("nodes": [{"mesh": 0}])",
         R"("nodes": [{"mesh": 0, "matrix": [1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1]}])"},
        {R"("indices": 3})", R"("indices": 3, "targets": [{"POSITION": 0}]})"},
        {R"(, "indices": 3})", R"(})"},
        {R"("TEXCOORD_0": 2)", R"("TEXCOORD_1": 2)"},
        {R"("scenes": [{"nodes": [0]}])", R"("scenes": [{"nodes": [0,0]}])"},
        {R"("nodes": [{"mesh": 0}])",
         R"("nodes": [{"mesh": 0, "translation": [0,0,0], "matrix": [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}])"},
        {R"("bufferView": 3, "componentType": 5123)",
         R"("bufferView": 3, "sparse": {"count": 1, "indices": {"bufferView": 3, "componentType": 5123}, "values": {"bufferView": 3, "byteOffset": 2}}, "componentType": 5123)"},
    };
    for (const auto& [from, to] : mutations) {
        SCOPED_TRACE(to);
        WriteTwoPrimitiveGltf(input);
        ASSERT_TRUE(ReplaceGltf(input, from, to));
        {
            std::ofstream previous(output);
            previous << "last-valid-generation";
        }
        EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
        EXPECT_EQ(FileBytes(output), "last-valid-generation");
    }
}

TEST(StaticSceneImport, RefusesNonfiniteVertexAttributes) {
    const TempDirectory temp;
    const auto input = temp.path() / "nonfinite.gltf";
    const auto output = temp.path() / "nonfinite.lmesh";
    for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::max()}) {
        SCOPED_TRACE(value);
        WriteTwoPrimitiveGltf(input, value);
        EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
        EXPECT_FALSE(std::filesystem::exists(output));
    }
}

TEST(StaticSceneImport, RefusesStaticMorphTargetsAndGpuInstances) {
    const TempDirectory temp;
    const auto input = temp.path() / "deformed.gltf";
    const auto output = temp.path() / "deformed.lmesh";
    WriteTwoPrimitiveGltf(input);
    // Match target counts on both primitives so this reaches static conversion.
    ASSERT_TRUE(
        ReplaceGltf(input, R"("indices": 3})", R"("indices": 3, "targets": [{"POSITION": 0}]})"));
    ASSERT_TRUE(
        ReplaceGltf(input, R"("indices": 7})", R"("indices": 7, "targets": [{"POSITION": 4}]})"));
    EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
    EXPECT_FALSE(std::filesystem::exists(output));
    WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(
        input,
        R"("nodes": [{"mesh": 0}])",
        R"("nodes": [{"mesh": 0, "extensions": {"EXT_mesh_gpu_instancing": {"attributes": {"TRANSLATION": 0}}}}])"));
    EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(StaticSceneImport, ResolvesUnambiguousDocumentsWithoutDefaultScene) {
    const TempDirectory temp;
    const auto input = temp.path() / "library.gltf";
    const auto output = temp.path() / "library.lmesh";
    const auto expected = WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    const auto identityBytes = FileBytes(output);
    // A sole scene, an implicit set of node roots, and a mesh-only library all
    // retain legacy identity geometry, including the exact compiled layout.
    ASSERT_TRUE(ReplaceGltf(input,
                            R"(,
  "scene": 0)",
                            ""));
    for (const auto& member : {R"("scenes": [{"nodes": [0]}])", R"("nodes": [{"mesh": 0}])"}) {
        ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
        EXPECT_EQ(FileBytes(output), identityBytes);
        ASSERT_TRUE(ReplaceGltf(input, std::string(",\n  ") + member, ""));
    }
    ASSERT_TRUE(process_gltf_checked(input.string(), output.string()));
    EXPECT_EQ(FileBytes(output), identityBytes);
    ExpectVertices(ReadLmesh(output), expected);
}

TEST(StaticSceneImport, RefusesAmbiguousScenesAndEmptySelectedScene) {
    const TempDirectory temp;
    const auto input = temp.path() / "scene.gltf";
    const auto output = temp.path() / "scene.lmesh";
    WriteTwoPrimitiveGltf(input);
    ASSERT_TRUE(ReplaceGltf(
        input, R"("scenes": [{"nodes": [0]}])", R"("scenes": [{"nodes": []}, {"nodes": [0]}])"));
    EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
    EXPECT_FALSE(std::filesystem::exists(output));
    ASSERT_TRUE(ReplaceGltf(input,
                            R"(,
  "scene": 0)",
                            ""));
    EXPECT_FALSE(process_gltf_checked(input.string(), output.string()));
    EXPECT_FALSE(std::filesystem::exists(output));
}

// ---------------------------------------------------------------------------
//.ltex texture round-trip
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kLtexMagic = 0x5845544C; // 'LTEX' little-endian
constexpr uint16_t kLtexVersionExpected = 1;

struct LtexHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t mip_count;
    uint32_t width;
    uint32_t height;
    uint8_t channels;
};

struct LtexMipLevel {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;
};

struct LtexImage {
    LtexHeader header{};
    std::vector<LtexMipLevel> mips;
};

// Reads the.ltex layout written by asset_processor's WriteLtex (field-by-field
// header, then the raw mip chain).
LtexImage ReadLtex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    LtexImage image;
    in.read(reinterpret_cast<char*>(&image.header.magic), sizeof(image.header.magic));
    in.read(reinterpret_cast<char*>(&image.header.version), sizeof(image.header.version));
    in.read(reinterpret_cast<char*>(&image.header.mip_count), sizeof(image.header.mip_count));
    in.read(reinterpret_cast<char*>(&image.header.width), sizeof(image.header.width));
    in.read(reinterpret_cast<char*>(&image.header.height), sizeof(image.header.height));
    in.read(reinterpret_cast<char*>(&image.header.channels), sizeof(image.header.channels));

    uint32_t w = image.header.width;
    uint32_t h = image.header.height;
    for (uint16_t level = 0; level < image.header.mip_count; ++level) {
        LtexMipLevel mip;
        mip.width = w;
        mip.height = h;
        const size_t byte_count = static_cast<size_t>(w) * h * image.header.channels;
        mip.pixels.resize(byte_count);
        in.read(reinterpret_cast<char*>(mip.pixels.data()),
                static_cast<std::streamsize>(byte_count));
        image.mips.push_back(std::move(mip));
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
    }
    return image;
}

std::filesystem::path TestTexturePath(const char* name) {
    return std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "textures" / "test" / name;
}

// Expected mip count for a (w,h) base: levels until both dims reach 1.
uint16_t ExpectedMipCount(uint32_t w, uint32_t h) {
    uint16_t count = 1;
    while (w > 1u || h > 1u) {
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
        ++count;
    }
    return count;
}

} // namespace

TEST(LtexRoundTrip, ImportsPngAndPreservesMip0Pixels) {
    static_assert(sizeof(LtexHeader::magic) == 4);

    const std::filesystem::path source = TestTexturePath("checker_16.png");
    ASSERT_TRUE(std::filesystem::exists(source)) << source.string();

    TempDirectory temp;
    const std::filesystem::path output = temp.path() / "checker_16.ltex";
    ASSERT_TRUE(process_texture(source.string(), output.string()));
    ASSERT_TRUE(std::filesystem::exists(output));

    const LtexImage image = ReadLtex(output);
    EXPECT_EQ(image.header.magic, kLtexMagic);
    EXPECT_EQ(image.header.version, kLtexVersionExpected);
    EXPECT_EQ(image.header.channels, 4u);
    EXPECT_EQ(image.header.width, 16u);
    EXPECT_EQ(image.header.height, 16u);

    // Load the source PNG the same way the importer does (no flip, force RGBA)
    // and assert byte-for-byte equality at mip 0.
    int w = 0, h = 0, c = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* png = stbi_load(source.string().c_str(), &w, &h, &c, 4);
    ASSERT_NE(png, nullptr);
    ASSERT_EQ(w, 16);
    ASSERT_EQ(h, 16);

    ASSERT_FALSE(image.mips.empty());
    const LtexMipLevel& mip0 = image.mips.front();
    ASSERT_EQ(mip0.width, 16u);
    ASSERT_EQ(mip0.height, 16u);
    ASSERT_EQ(mip0.pixels.size(), static_cast<size_t>(16 * 16 * 4));
    EXPECT_EQ(0, std::memcmp(mip0.pixels.data(), png, mip0.pixels.size()));
    stbi_image_free(png);
}

TEST(LtexRoundTrip, BuildsHalvingMipChainDownToOne) {
    const std::filesystem::path source = TestTexturePath("checker_16.png");
    ASSERT_TRUE(std::filesystem::exists(source)) << source.string();

    TempDirectory temp;
    const std::filesystem::path output = temp.path() / "checker_16.ltex";
    ASSERT_TRUE(process_texture(source.string(), output.string()));

    const LtexImage image = ReadLtex(output);
    // 16x16 -> 8 -> 4 -> 2 -> 1 == 5 levels.
    EXPECT_EQ(image.header.mip_count, ExpectedMipCount(16u, 16u));
    ASSERT_EQ(image.mips.size(), image.header.mip_count);

    uint32_t expected_w = 16u;
    uint32_t expected_h = 16u;
    for (const LtexMipLevel& mip : image.mips) {
        EXPECT_EQ(mip.width, expected_w);
        EXPECT_EQ(mip.height, expected_h);
        EXPECT_EQ(mip.pixels.size(),
                  static_cast<size_t>(expected_w) * expected_h * image.header.channels);
        expected_w = std::max(1u, expected_w / 2u);
        expected_h = std::max(1u, expected_h / 2u);
    }
    const LtexMipLevel& smallest = image.mips.back();
    EXPECT_EQ(smallest.width, 1u);
    EXPECT_EQ(smallest.height, 1u);
}

TEST(LtexRoundTrip, NonSquareTextureHalvesEachDimensionIndependently) {
    const std::filesystem::path source = TestTexturePath("framed_32x8.png");
    ASSERT_TRUE(std::filesystem::exists(source)) << source.string();

    TempDirectory temp;
    const std::filesystem::path output = temp.path() / "framed_32x8.ltex";
    ASSERT_TRUE(process_texture(source.string(), output.string()));

    const LtexImage image = ReadLtex(output);
    EXPECT_EQ(image.header.width, 32u);
    EXPECT_EQ(image.header.height, 8u);
    // 32x8 -> 16x4 -> 8x2 -> 4x1 -> 2x1 -> 1x1 == 6 levels (width still halving
    // after height floors at 1).
    EXPECT_EQ(image.header.mip_count, ExpectedMipCount(32u, 8u));
    ASSERT_EQ(image.mips.size(), image.header.mip_count);

    // Height must floor at 1 and stay there while width keeps halving.
    EXPECT_EQ(image.mips.back().width, 1u);
    EXPECT_EQ(image.mips.back().height, 1u);
    // Level 3 (0-indexed) should be 4x1: height already floored.
    ASSERT_GE(image.mips.size(), 4u);
    EXPECT_EQ(image.mips[3].width, 4u);
    EXPECT_EQ(image.mips[3].height, 1u);
}

TEST(LtexRoundTrip, GradientMip0RoundTripsExactly) {
    const std::filesystem::path source = TestTexturePath("gradient_16.png");
    ASSERT_TRUE(std::filesystem::exists(source)) << source.string();

    TempDirectory temp;
    const std::filesystem::path output = temp.path() / "gradient_16.ltex";
    ASSERT_TRUE(process_texture(source.string(), output.string()));

    const LtexImage image = ReadLtex(output);
    int w = 0, h = 0, c = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* png = stbi_load(source.string().c_str(), &w, &h, &c, 4);
    ASSERT_NE(png, nullptr);
    ASSERT_FALSE(image.mips.empty());
    ASSERT_EQ(image.mips.front().pixels.size(), static_cast<size_t>(w) * h * 4);
    EXPECT_EQ(0,
              std::memcmp(image.mips.front().pixels.data(), png, image.mips.front().pixels.size()));
    stbi_image_free(png);
}

TEST(LtexRoundTrip, MaterialColorMipsFilterInLinearLight) {
    TempDirectory temp;
    const auto input = temp.path() / "contrast.png";
    const auto output = temp.path() / "contrast.ltex";
    const std::array<uint8_t, 16> pixels = {
        0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    ASSERT_NE(stbi_write_png(input.string().c_str(), 2, 2, 4, pixels.data(), 8), 0);
    ASSERT_EQ(Import({input.string(), output.string(), "--srgb"}), 0);
    const auto image = ReadLtex(output);
    ASSERT_EQ(image.mips.size(), 2u);
    for (int channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(image.mips.back().pixels[channel], 188, 1);
    EXPECT_EQ(image.mips.back().pixels[3], 255);
    ASSERT_EQ(Import({input.string(), output.string(), "--linear"}), 0);
    const auto linear = ReadLtex(output);
    for (int channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(linear.mips.back().pixels[channel], 128, 1);
}

TEST(LtexRoundTrip, NormalMipsStayLinearAndUnitLength) {
    TempDirectory temp;
    const auto input = temp.path() / "normal.png";
    const auto output = temp.path() / "normal.ltex";
    const std::array<uint8_t, 16> pixels = {
        255, 128, 128, 255, 128, 255, 128, 255, 255, 128, 128, 255, 128, 255, 128, 255};
    ASSERT_NE(stbi_write_png(input.string().c_str(), 2, 2, 4, pixels.data(), 8), 0);
    ASSERT_EQ(Import({input.string(), output.string(), "--normal-map"}), 0);
    const auto image = ReadLtex(output);
    ASSERT_EQ(image.mips.size(), 2u);
    const auto& normal = image.mips.back().pixels;
    EXPECT_NEAR(normal[0], 218, 1);
    EXPECT_NEAR(normal[1], 218, 1);
    EXPECT_NEAR(normal[2], 128, 1);
}

TEST(LtexRoundTrip, LeafCutoutUsesAuthoredOpacityRegardlessOfColor) {
    TempDirectory temp;
    const auto input = temp.path() / "albedo.png";
    const auto mask = temp.path() / "mask.png";
    const auto output = temp.path() / "leaf.ltex";
    // All-black leaf color must retain opaque portions of its authored mask.
    std::array<uint8_t, 8 * 8 * 4> pixels{};
    std::array<uint8_t, 8 * 8> opacity{};
    for (size_t i = 0; i < opacity.size(); ++i) {
        pixels[i * 4 + 3] = 255;
        opacity[i] = i % 8 < 4 ? 255 : 0;
    }
    ASSERT_NE(stbi_write_png(input.string().c_str(), 8, 8, 4, pixels.data(), 32), 0);
    ASSERT_NE(stbi_write_png(mask.string().c_str(), 8, 8, 1, opacity.data(), 8), 0);
    ASSERT_EQ(Import({input.string(), output.string(), "--alpha-mask", mask.string()}), 0);
    const auto image = ReadLtex(output);
    ASSERT_EQ(image.mips.size(), 4u);
    for (const auto& mip : image.mips) {
        if (mip.width == 1)
            continue; // A single texel cannot express half coverage with a binary cutoff.
        size_t covered = 0;
        for (size_t i = 3; i < mip.pixels.size(); i += 4)
            covered += mip.pixels[i] >= 128;
        EXPECT_EQ(covered, static_cast<size_t>(mip.width * mip.height / 2));
    }
    EXPECT_NE(Import({input.string(), output.string(), "--alpha-mask", "missing.png"}), 0);
}
