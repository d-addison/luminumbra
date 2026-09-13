#include "authoring/PrefabDigest.h"
#include "authoring/PrefabRuntime.h"
#include "components/CoreComponents.h"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>

namespace {
using namespace Luminumbra::Authoring;
using Json = nlohmann::json;
constexpr auto kGeneration = "11111111111111111111111111111111";
constexpr auto kMesh = "m-22222222222222222222222222222222.lmesh";
constexpr auto kTexture = "t-33333333333333333333333333333333.ltex";

std::vector<std::uint8_t> Bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

void Unsigned(std::vector<std::uint8_t>& bytes, std::uint32_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Json Matrix(const glm::dmat4& value = glm::dmat4(1.0)) {
    auto result = Json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            result.push_back(value[c][r]);
    return result;
}

Json Node(const std::string& id, Json parent = nullptr, const glm::dmat4& local = glm::dmat4(1.0)) {
    return {{"id", id},
            {"label", id},
            {"parent", parent},
            {"local_matrix", Matrix(local)},
            {"reverse_front_face", false},
            {"mesh", "mesh.shared"}};
}

class PrefabRuntimeTest : public ::testing::Test {
protected:
    std::filesystem::path project;
    std::filesystem::path generation;
    Json descriptor;
    Json manifest;
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::string digest;

    void SetUp() override {
        static std::atomic<unsigned> sequence{0};
        project = std::filesystem::temp_directory_path() /
                  ("luminumbra-prefab-test-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                   "-" + std::to_string(sequence++));
        generation = project / ".luminumbra-author" / "generations" / kGeneration;
        std::filesystem::create_directories(generation);
        auto& mesh = files[kMesh];
        Unsigned(mesh, 0x48534d4c);
        Unsigned(mesh, 3);
        Unsigned(mesh, 3);
        for (float value : {0.f, 0.f, 0.f, 2.f, -1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f,  0.f,
                            0.f, 0.f, 0.f, 1.f, 1.f,  0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.5f, 1.f})
            Unsigned(mesh, std::bit_cast<std::uint32_t>(value));
        for (unsigned index : {0, 1, 2})
            Unsigned(mesh, index);
        auto& texture = files[kTexture];
        Unsigned(texture, 0x5845544c);
        Unsigned(texture, 1, 2);
        Unsigned(texture, 1, 2);
        Unsigned(texture, 1);
        Unsigned(texture, 1);
        Unsigned(texture, 4, 1);
        for (unsigned byte : {255, 128, 0, 200})
            Unsigned(texture, byte, 1);
        const Json texture_binding = {
            {"file", kTexture},
            {"encoding", "srgb"},
            {"sampler",
             {{"wrapS", 10497}, {"wrapT", 33071}, {"magFilter", 9729}, {"minFilter", 9987}}},
            {"coordinates",
             {{"set", 1}, {"offset", {0.25, 0.5}}, {"scale", {2, 3}}, {"rotation", 0.2}}}};
        const Json material = {{"base_color", {0.8, 0.7, 0.6, 0.9}},
                               {"metallic", 0.4},
                               {"roughness", 0.3},
                               {"emissive", {0.1, 0.2, 0.3}},
                               {"alpha_mode", "MASK"},
                               {"alpha_cutoff", 0.42},
                               {"double_sided", true},
                               {"normal_scale", 0.75},
                               {"occlusion_strength", 0.8},
                               {"textures", {{"baseColorTexture", texture_binding}}}};
        auto parent = Node("z.parent", nullptr, glm::scale(glm::dmat4(1.0), glm::dvec3(2, 1, 1)));
        parent.erase("mesh");
        auto child =
            Node("a.child", "z.parent", glm::rotate(glm::dmat4(1.0), 0.6, glm::dvec3(0, 0, 1)));
        auto mirror = Node("m.mirror", nullptr, glm::scale(glm::dmat4(1.0), glm::dvec3(-1, 1, 1)));
        mirror["reverse_front_face"] = true;
        descriptor = {{"schema", "luminumbra.asset.prefab.v1"},
                      {"asset_id", "asset.prop"},
                      {"coordinates", "gltf-rh-y-up-meters"},
                      {"runtime_components", false},
                      {"nodes", Json::array({child, mirror, parent})},
                      {"meshes",
                       {{"mesh.shared",
                         Json::array({{{"file", kMesh},
                                       {"material", "mat.copper"},
                                       {"uv_transform_baked", false}}})}}},
                      {"materials", {{"mat.copper", material}}}};
        manifest = {
            {"schema", "luminumbra.authoring.generation.v1"},
            {"mode", "NATIVE"},
            {"job_id", kGeneration},
            {"asset_id", "asset.prop"},
            {"revision", 1},
            {"build_identity", std::string(64, 'a')},
            {"input_hashes", Json::object()},
            {"outputs",
             {{kMesh, {{"format", "LMSH"}, {"vertices", 3}, {"triangles", 1}, {"joints", 0}}},
              {kTexture,
               {{"format", "LTEX"},
                {"width", 1},
                {"height", 1},
                {"channels", 4},
                {"mip_levels", 1}}}}}};
        Publish();
    }

    void TearDown() override {
        std::filesystem::remove_all(project);
    }

    void Write(const std::string& name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(generation / name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }

    void Publish() {
        files["prefab.json"] = Bytes(descriptor.dump());
        manifest["outputs"]["prefab.json"] = {{"format", "PREFAB"},
                                              {"schema", "luminumbra.asset.prefab.v1"},
                                              {"nodes", descriptor.at("nodes").size()},
                                              {"meshes", descriptor.at("meshes").size()},
                                              {"materials", descriptor.at("materials").size()}};
        for (const auto& [name, bytes] : files) {
            Write(name, bytes);
            manifest["outputs"][name]["sha256"] = Detail::Sha256(bytes);
            manifest["outputs"][name]["bytes"] = bytes.size();
        }
        PublishManifest();
    }

    void PublishManifest() {
        const auto bytes = Bytes(manifest.dump());
        Write("manifest.json", bytes);
        digest = Detail::Sha256(bytes);
    }

    std::shared_ptr<const PrefabAsset> Load() {
        return PrefabAsset::Load(project, kGeneration, digest);
    }
};

TEST(PrefabDigest, MatchesIndependentSha256Vectors) {
    EXPECT_EQ(Detail::Sha256(Bytes("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Detail::Sha256(Bytes("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Detail::Sha256(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(Detail::Sha256(Bytes(std::string(1000000, 'a'))),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_F(PrefabRuntimeTest, LoadsOwnedSharedGeometryAndExactMaterialBindings) {
    const auto asset = Load();
    ASSERT_EQ(asset->nodes().size(), 3u);
    EXPECT_EQ(asset->nodes()[0].id, "z.parent");
    const auto& child = asset->nodes()[1];
    const auto& mirror = asset->nodes()[2];
    ASSERT_EQ(child.draws.size(), 1u);
    EXPECT_EQ(child.draws[0].mesh, mirror.draws[0].mesh);
    EXPECT_EQ(child.draws[0].material, mirror.draws[0].material);
    EXPECT_EQ(child.draws[0].mesh->bytes, files.at(kMesh));
    EXPECT_EQ(child.draws[0].material->properties, descriptor["materials"]["mat.copper"]);
    EXPECT_EQ(child.draws[0].material->textures.at("baseColorTexture")->bytes, files.at(kTexture));
    std::filesystem::remove_all(generation);
    EXPECT_EQ(child.draws[0].mesh->bytes.size(),
              136u); // Pinned owned bytes outlive the generation directory.
}

TEST_F(PrefabRuntimeTest, InstantiatesRealHierarchyShearNormalsMirroringAndWorldBounds) {
    const auto asset = Load();
    entt::registry registry;
    PrefabScene scene(registry);
    const auto placement = glm::translate(glm::dmat4(1.0), glm::dvec3(7, 8, 9));
    scene.Replace("placed.one", asset, placement);
    const auto report = scene.Inspect("placed.one");
    EXPECT_EQ(report["nodes"][0]["children"], Json::array({"a.child"}));
    EXPECT_EQ(report["nodes"][1]["parent"], "z.parent");
    auto view = registry.view<const PrefabNodeComponent>();
    ASSERT_EQ(view.size(), 3u);
    for (const auto entity : view) {
        const auto& component = view.get<const PrefabNodeComponent>(entity);
        if (component.node_id == "a.child") {
            EXPECT_GT(std::abs(glm::dot(glm::dvec3(component.world_matrix[0]),
                                        glm::dvec3(component.world_matrix[1]))),
                      0.1);
            const glm::dvec3 tangent = glm::dmat3(component.world_matrix) * glm::dvec3(1, 0, 0);
            const glm::dvec3 normal = component.normal_matrix * glm::dvec3(0, 1, 0);
            EXPECT_NEAR(glm::dot(tangent, normal), 0, 1e-12);
            const auto& bounds = component.draw_bounds[0];
            for (const glm::dvec4 vertex :
                 {glm::dvec4(-1, 0, 0, 1), glm::dvec4(1, 0, 0, 1), glm::dvec4(0, 1, 0, 1)}) {
                const glm::dvec3 point(component.world_matrix * vertex);
                for (int axis = 0; axis < 3; ++axis) {
                    EXPECT_GE(point[axis], bounds.minimum[axis]);
                    EXPECT_LE(point[axis], bounds.maximum[axis]);
                }
            }
        }
        if (component.node_id == "m.mirror") {
            EXPECT_TRUE(component.reverse_front_face);
        }
    }
    const auto mirrored_placement = glm::scale(placement, glm::dvec3(-1, 1, 1));
    scene.Replace("placed.two", asset, mirrored_placement);
    EXPECT_FALSE(scene.Inspect("placed.two")["nodes"][2]["reverse_front_face"].get<bool>());
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 6u);
}

TEST_F(PrefabRuntimeTest, EqualCountReplacementRefreshesTransformsAndStableIdentities) {
    entt::registry registry;
    PrefabScene scene(registry);
    const auto placement = glm::translate(glm::dmat4(1.0), glm::dvec3(7, 8, 9));
    scene.Replace("instance.one", Load(), placement);
    const auto before = scene.Inspect("instance.one");
    descriptor["nodes"][2]["local_matrix"][12] = 42;
    Publish();
    scene.Replace("instance.one", Load());
    const auto after = scene.Inspect("instance.one");
    EXPECT_EQ(before["nodes"].size(), after["nodes"].size());
    EXPECT_EQ(after["revision"], 2);
    EXPECT_EQ(after["nodes"][1]["world_matrix"][12], 49);
    EXPECT_EQ(after["placement_matrix"], Matrix(placement));
    EXPECT_EQ(after["nodes"][1]["mesh"], "mesh.shared");
    EXPECT_EQ(after["nodes"][1]["id"], before["nodes"][1]["id"]);
    EXPECT_NE(after["manifest_sha256"], before["manifest_sha256"]);
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 3u);
    EXPECT_TRUE(scene.Remove("instance.one"));
    EXPECT_FALSE(scene.Remove("instance.one"));
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 0u);
}

TEST_F(PrefabRuntimeTest, FailedReloadAndInvalidPlacementRetainPreviousInstance) {
    entt::registry registry;
    PrefabScene scene(registry);
    const auto asset = Load();
    scene.Replace("kept.instance", asset);
    const auto before = scene.Inspect("kept.instance");
    Write(kMesh, Bytes("corrupt"));
    EXPECT_THROW(scene.Replace("kept.instance", Load()), std::runtime_error);
    EXPECT_THROW(scene.Replace("kept.instance", asset, glm::dmat4(0.0)), std::runtime_error);
    EXPECT_EQ(scene.Inspect("kept.instance"), before);
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 3u);
}

struct ThrowConstruction {
    void OnNode(entt::registry&, entt::entity) {
        throw std::runtime_error("injected component failure");
    }
};

TEST_F(PrefabRuntimeTest, RegistryConstructionFailureRollsBackOnlyCandidateEntities) {
    entt::registry registry;
    PrefabScene scene(registry);
    const auto asset = Load();
    scene.Replace("kept.instance", asset);
    const auto before = scene.Inspect("kept.instance");
    ThrowConstruction callback;
    registry.on_construct<PrefabNodeComponent>().connect<&ThrowConstruction::OnNode>(callback);
    EXPECT_THROW(scene.Replace("kept.instance", asset), std::runtime_error);
    EXPECT_EQ(scene.Inspect("kept.instance"), before);
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 3u);
    registry.on_construct<PrefabNodeComponent>().disconnect<&ThrowConstruction::OnNode>(callback);
}

TEST_F(PrefabRuntimeTest, DestructionRemovesOnlyOwnedEntities) {
    entt::registry registry;
    const auto unrelated = registry.create();
    {
        PrefabScene scene(registry);
        scene.Replace("owned.instance", Load());
        EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 3u);
    }
    EXPECT_EQ(registry.view<const PrefabNodeComponent>().size(), 0u);
    EXPECT_TRUE(registry.valid(unrelated));
}

TEST_F(PrefabRuntimeTest, RefusesUnpinnedOrArbitraryGenerationPaths) {
    for (const auto id : {"../escape", "/tmp/generation", "C:\\generation", "current", "1111/1111"})
        EXPECT_THROW(PrefabAsset::Load(project, id, digest), std::runtime_error);
    EXPECT_THROW(PrefabAsset::Load(project, kGeneration, std::string(64, '0')), std::runtime_error);
    EXPECT_THROW(PrefabAsset::Load(project, kGeneration, ""), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesMissingAndUnlistedGenerationMembers) {
    std::filesystem::remove(generation / kMesh);
    EXPECT_THROW(Load(), std::runtime_error);
    Publish();
    Write("extra.lmesh", files[kMesh]);
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesTraversalAndLinksInReferencedMembers) {
    manifest["outputs"]["../outside.lmesh"] = manifest["outputs"][kMesh];
    PublishManifest();
    EXPECT_THROW(Load(), std::runtime_error);
    manifest["outputs"].erase("../outside.lmesh");
    PublishManifest();
}

#ifndef _WIN32
// POSIX link refusal is exercised directly. Windows reparse refusal requires
// the separate host qualification; this does not silently pass on permission denial.
TEST_F(PrefabRuntimeTest, RefusesSymlinkedMembersAndGenerationAncestors) {
    const auto original = project / "outside.lmesh";
    std::filesystem::rename(generation / kMesh, original);
    std::filesystem::create_symlink(original, generation / kMesh);
    EXPECT_THROW(Load(), std::runtime_error);
    std::filesystem::remove(generation / kMesh);
    std::filesystem::rename(original, generation / kMesh);
    const auto moved = project / "moved-generation";
    std::filesystem::rename(generation, moved);
    std::filesystem::create_directory_symlink(moved, generation);
    EXPECT_THROW(Load(), std::runtime_error);
}
#endif

TEST_F(PrefabRuntimeTest, RefusesUnknownSchemaComponentsAndMaterialFields) {
    const auto original = descriptor;
    descriptor["schema"] = "future.schema";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["runtime_components"] = true;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["materials"]["mat.copper"]["clearcoat"] = 1;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesMissingMeshMaterialAndTextureReferences) {
    const auto original = descriptor;
    descriptor["nodes"][0]["mesh"] = "mesh.missing";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["meshes"]["mesh.shared"][0]["material"] = "material.missing";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["materials"]["mat.copper"]["textures"]["baseColorTexture"]["file"] =
        "texture.missing";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesCyclesMissingParentsAndDuplicateIds) {
    const auto original = descriptor;
    descriptor["nodes"][2]["parent"] = "a.child";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["nodes"][0]["parent"] = "missing.parent";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["nodes"][1]["id"] = "a.child";
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesSingularLocalShearAndIncorrectWinding) {
    const auto original = descriptor;
    descriptor["nodes"][2]["local_matrix"][0] = 0;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["nodes"][2]["local_matrix"][4] = 0.5;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    descriptor = original;
    descriptor["nodes"][1]["reverse_front_face"] = false;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesBadIndexAndNonfiniteMeshEvenWithMatchingHashes) {
    files[kMesh].back() = 0x7f;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    files[kMesh].back() = 0;
    files[kMesh][28] = 0;
    files[kMesh][29] = 0;
    files[kMesh][30] = 0x80;
    files[kMesh][31] = 0x7f;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesIncompleteTextureMipsAndManifestMetricDrift) {
    files[kTexture][6] = 2;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    files[kTexture][6] = 1;
    Publish();
    manifest["outputs"][kMesh]["vertices"] = 4;
    PublishManifest();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesInvalidSamplerAndMixedUvTransforms) {
    auto& material = descriptor["materials"]["mat.copper"];
    material["textures"]["baseColorTexture"]["sampler"]["wrapS"] = 123;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
    material["textures"]["baseColorTexture"]["sampler"]["wrapS"] = 10497;
    material["textures"]["emissiveTexture"] = material["textures"]["baseColorTexture"];
    material["textures"]["emissiveTexture"]["coordinates"]["set"] = 0;
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, RefusesDuplicateJsonKeysEvenWithMatchingPin) {
    const auto text = manifest.dump();
    const auto bytes = Bytes("{\"mode\":\"MOCK\"," + text.substr(1));
    Write("manifest.json", bytes);
    digest = Detail::Sha256(bytes);
    EXPECT_THROW(Load(), std::runtime_error);
}

TEST_F(PrefabRuntimeTest, Full4096NodeChainIsIterativeAndBounded) {
    descriptor["nodes"] = Json::array();
    for (unsigned i = 0; i < 4096; ++i) {
        auto node = Node("node." + std::to_string(i),
                         i == 4095 ? Json(nullptr) : Json("node." + std::to_string(i + 1)));
        if (i != 0)
            node.erase("mesh");
        descriptor["nodes"].push_back(std::move(node));
    }
    Publish();
    const auto asset = Load();
    EXPECT_EQ(asset->nodes().size(), 4096u);
    EXPECT_EQ(asset->nodes().front().id, "node.4095");
    EXPECT_EQ(asset->nodes().back().id, "node.0");
    descriptor["nodes"].push_back(Node("overflow.node"));
    Publish();
    EXPECT_THROW(Load(), std::runtime_error);
}
} // namespace
