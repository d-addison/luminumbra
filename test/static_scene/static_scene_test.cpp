#include "authoring/PrefabDigest.h"
#include "authoring/PrefabRuntime.h"
#include "components/CoreComponents.h"
#include <luminumbra/rendering/StaticScene.h>

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

class StaticSceneTest : public ::testing::Test {
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

using namespace Luminumbra::Rendering;
StaticMatrix4 PublicMatrix(const glm::dmat4& m) {
    StaticMatrix4 result;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            result[c * 4 + r] = m[c][r];
    return result;
}
TEST_F(StaticSceneTest, DecodesOwnedRawGeometryAndTrueMaterialBindings) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    auto snapshot = scene.Snapshot();
    ASSERT_EQ(snapshot->draws.size(), 2u);
    const auto& draw = snapshot->draws[0];
    EXPECT_EQ(draw.mesh->vertices.size(), 3u);
    EXPECT_EQ(draw.mesh->indices, (std::vector<std::uint32_t>{0, 1, 2}));
    EXPECT_EQ(draw.mesh->vertices[1].uv, (std::array<float, 2>{1, 0}));
    EXPECT_EQ(draw.material->alpha_mode, StaticAlphaMode::Mask);
    EXPECT_DOUBLE_EQ(draw.material->alpha_cutoff, .42);
    EXPECT_DOUBLE_EQ(draw.material->normal_scale, .75);
    EXPECT_DOUBLE_EQ(draw.material->occlusion_strength, .8);
    EXPECT_EQ(draw.material->emissive, (std::array<double, 3>{.1, .2, .3}));
    const auto& binding = draw.material->textures[0];
    ASSERT_TRUE(binding.texture);
    EXPECT_EQ(binding.source_uv_set, 1u);
    EXPECT_EQ(binding.scale, (std::array<double, 2>{2, 3}));
    EXPECT_EQ(binding.sampler.wrap_t, 33071u);
    EXPECT_EQ(binding.sampler.min_filter, 9987u);
    EXPECT_EQ(binding.texture->encoding, StaticEncoding::Srgb);
    EXPECT_EQ(binding.texture->mips[0].rgba8, (std::vector<std::uint8_t>{255, 128, 0, 200}));
    EXPECT_EQ(draw.mesh, snapshot->draws[1].mesh);
    EXPECT_EQ(draw.material, snapshot->draws[1].material);
}
TEST_F(StaticSceneTest, TransformBatchPreservesGeometryAndImmutableOldSnapshot) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    auto old = scene.Snapshot();
    const auto local = PublicMatrix(glm::translate(glm::dmat4(1), glm::dvec3(7, 0, 0)));
    const std::array updates{StaticLocalMatrixUpdate{"instance.one", "z.parent", local}};
    std::filesystem::remove_all(generation); // No later filesystem access is permitted.
    scene.UpdateLocalMatrices(updates, 1);
    auto next = scene.Snapshot();
    EXPECT_EQ(next->revision, 2u);
    EXPECT_EQ(old->revision, 1u);
    EXPECT_EQ(next->draws[0].key, old->draws[0].key);
    EXPECT_EQ(next->draws[0].mesh, old->draws[0].mesh);
    EXPECT_EQ(next->draws[0].material, old->draws[0].material);
    EXPECT_NE(next->draws[0].model, old->draws[0].model);
    EXPECT_DOUBLE_EQ(next->draws[0].model[12], 7);
    EXPECT_EQ(next->draws[1].model, old->draws[1].model);
}
TEST_F(StaticSceneTest, ParentShearMirrorNormalAndBoundsRemainFullAffine) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    auto snapshot = scene.Snapshot();
    const auto original = Load();
    for (const auto& draw : snapshot->draws) {
        const auto found = std::find_if(original->nodes().begin(),
                                        original->nodes().end(),
                                        [&](const auto& n) { return n.id == draw.key.node_id; });
        ASSERT_NE(found, original->nodes().end());
        EXPECT_EQ(draw.model, PublicMatrix(found->world_matrix));
        const auto normal = glm::transpose(glm::inverse(glm::dmat3(found->world_matrix)));
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                EXPECT_DOUBLE_EQ(draw.normal[c * 3 + r], normal[c][r]);
        EXPECT_EQ(draw.reverse_front_face, draw.key.node_id == "m.mirror");
        for (const auto& vertex : draw.mesh->vertices) {
            const auto p =
                found->world_matrix *
                glm::dvec4(vertex.position[0], vertex.position[1], vertex.position[2], 1);
            for (int axis = 0; axis < 3; ++axis) {
                EXPECT_LE(draw.world_bounds.minimum[axis], p[axis]);
                EXPECT_GE(draw.world_bounds.maximum[axis], p[axis]);
            }
        }
    }
}
TEST_F(StaticSceneTest, InvalidBatchAndStaleRequestsLeaveSnapshotIdentity) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    auto before = scene.Snapshot();
    std::array updates{StaticLocalMatrixUpdate{"instance.one", "z.parent", kStaticIdentity},
                       StaticLocalMatrixUpdate{"instance.one", "missing", kStaticIdentity}};
    EXPECT_THROW(scene.UpdateLocalMatrices(updates, 1), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
    updates[1] = updates[0];
    EXPECT_THROW(scene.UpdateLocalMatrices(updates, 1), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
    EXPECT_THROW(scene.Remove("instance.one", 0), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
    updates[0].local[0] = 0;
    EXPECT_THROW(scene.UpdateLocalMatrices(std::span(updates).first(1), 1), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
    updates[0].local[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(scene.UpdateLocalMatrices(std::span(updates).first(1), 1), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
}
TEST_F(StaticSceneTest, EqualCountReplacementAndRemovalRetainOldGeneration) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    auto before = scene.Snapshot();
    descriptor["materials"]["mat.copper"]["base_color"][0] = .2;
    Publish();
    auto changed = StaticPrefab::Load(project.string(), kGeneration, digest);
    scene.Replace("instance.one", changed, kStaticIdentity, 1);
    auto after = scene.Snapshot();
    EXPECT_EQ(before->draws.size(), after->draws.size());
    EXPECT_EQ(before->draws[0].key, after->draws[0].key);
    EXPECT_NE(before->draws[0].material->identity, after->draws[0].material->identity);
    EXPECT_DOUBLE_EQ(before->draws[0].material->base_color[0], .8);
    EXPECT_DOUBLE_EQ(after->draws[0].material->base_color[0], .2);
    scene.Remove("instance.one", 2);
    EXPECT_TRUE(scene.Snapshot()->draws.empty());
    EXPECT_EQ(before->draws.size(), 2u);
}
TEST_F(StaticSceneTest, BlendAndFloatOverflowMaterialsRefuseBeforePublish) {
    descriptor["materials"]["mat.copper"]["alpha_mode"] = "BLEND";
    Publish();
    EXPECT_THROW(StaticPrefab::Load(project.string(), kGeneration, digest), std::runtime_error);
    descriptor["materials"]["mat.copper"]["alpha_mode"] = "OPAQUE";
    descriptor["materials"]["mat.copper"]["normal_scale"] = 1e100;
    Publish();
    EXPECT_THROW(StaticPrefab::Load(project.string(), kGeneration, digest), std::runtime_error);
}
TEST_F(StaticSceneTest, TextureRolesKeepDistinctEncodingAndAllAuthoredMips) {
    auto binding = descriptor["materials"]["mat.copper"]["textures"]["baseColorTexture"];
    for (const auto role :
         {"normalTexture", "metallicRoughnessTexture", "occlusionTexture", "emissiveTexture"}) {
        binding["encoding"] = std::string(role) == "emissiveTexture" ? "srgb" : "linear";
        descriptor["materials"]["mat.copper"]["textures"][role] = binding;
    }
    auto& texture = files[kTexture];
    texture.clear();
    Unsigned(texture, 0x5845544c);
    Unsigned(texture, 1, 2);
    Unsigned(texture, 2, 2);
    Unsigned(texture, 2);
    Unsigned(texture, 2);
    Unsigned(texture, 4, 1);
    for (unsigned i = 0; i < 20; ++i)
        Unsigned(texture, i, 1);
    manifest["outputs"][kTexture]["width"] = 2;
    manifest["outputs"][kTexture]["height"] = 2;
    manifest["outputs"][kTexture]["mip_levels"] = 2;
    Publish();
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("instance.one", asset, kStaticIdentity, 0);
    const auto& maps = scene.Snapshot()->draws[0].material->textures;
    for (const auto& map : maps) {
        ASSERT_TRUE(map.texture);
        ASSERT_EQ(map.texture->mips.size(), 2u);
        EXPECT_EQ(map.texture->mips[1].rgba8, (std::vector<std::uint8_t>{16, 17, 18, 19}));
    }
    EXPECT_EQ(maps[0].texture, maps[4].texture);
    EXPECT_EQ(maps[1].texture, maps[2].texture);
    EXPECT_NE(maps[0].texture, maps[1].texture);
    EXPECT_NE(maps[0].texture->identity, maps[1].texture->identity);
}
TEST_F(StaticSceneTest, EmptyBatchIsNoOpAndSceneLifetimeDoesNotInvalidateSnapshot) {
    std::shared_ptr<const StaticDrawSnapshot> retained;
    {
        StaticScene scene;
        scene.Replace("instance.one",
                      StaticPrefab::Load(project.string(), kGeneration, digest),
                      kStaticIdentity,
                      0);
        retained = scene.Snapshot();
        scene.UpdateLocalMatrices({}, 1);
        EXPECT_EQ(retained, scene.Snapshot());
    }
    EXPECT_EQ(retained->draws[0].mesh->vertices.size(), 3u);
    EXPECT_FALSE(retained->draws[0].generation->manifest_sha256().empty());
}
TEST_F(StaticSceneTest, UnusableNormalsRefuseBeforeSnapshotPublication) {
    for (const float value : {0.f, 1e30f}) {
        auto& bytes = files[kMesh];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto word = std::bit_cast<std::uint32_t>(value);
            for (unsigned b = 0; b < 4; ++b)
                bytes[28 + 12 + axis * 4 + b] = static_cast<std::uint8_t>(word >> (8 * b));
        }
        Publish();
        EXPECT_THROW(StaticPrefab::Load(project.string(), kGeneration, digest), std::runtime_error);
    }
}
TEST_F(StaticSceneTest, InstanceBudgetRefusalPreservesLastCompleteSnapshot) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    for (unsigned i = 0; i < 64; ++i)
        scene.Replace("instance." + std::to_string(i), asset, kStaticIdentity, i);
    auto before = scene.Snapshot();
    EXPECT_EQ(before->draws.size(), 128u);
    EXPECT_THROW(scene.Replace("instance.extra", asset, kStaticIdentity, 64), std::runtime_error);
    EXPECT_EQ(before, scene.Snapshot());
}
} // namespace

TEST_F(StaticSceneTest, CompleteReplacementCommitsOnceAndRemovesAbsentInstances) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    auto description = [&](const std::string& id) {
        StaticInstanceDescription value{id, asset, kStaticIdentity, {}};
        for (const auto& node : descriptor.at("nodes"))
            value.locals.push_back({node.at("id"), node.at("local_matrix").get<StaticMatrix4>()});
        return value;
    };
    std::vector instances{description("one"), description("two")};
    scene.ReplaceAll(instances, 0);
    const auto old = scene.Snapshot();
    ASSERT_EQ(old->revision, 1u);
    ASSERT_EQ(old->draws.size(), 4u);
    instances.resize(1);
    instances[0].locals[0].local[12] += 7;
    scene.ReplaceAll(instances, 1);
    const auto next = scene.Snapshot();
    EXPECT_EQ(next->revision, 2u);
    ASSERT_EQ(next->draws.size(), 2u);
    EXPECT_EQ(next->draws[0].mesh, old->draws[0].mesh);
    EXPECT_NE(next->draws[0].model, old->draws[0].model);
    scene.ReplaceAll({}, 2);
    EXPECT_TRUE(scene.Snapshot()->draws.empty());
    EXPECT_EQ(old->draws.size(), 4u);
}
TEST_F(StaticSceneTest,
       CompleteReplacementRefusesPartialUnknownDuplicateAndInvalidWithoutMutation) {
    auto asset = StaticPrefab::Load(project.string(), kGeneration, digest);
    StaticScene scene;
    scene.Replace("old", asset, kStaticIdentity, 0);
    const auto old = scene.Snapshot();
    StaticInstanceDescription description{"new", asset, kStaticIdentity, {}};
    for (const auto& node : descriptor.at("nodes"))
        description.locals.push_back({node.at("id"), node.at("local_matrix").get<StaticMatrix4>()});
    auto check = [&](StaticInstanceDescription bad) {
        const std::array instances{std::move(bad)};
        EXPECT_THROW(scene.ReplaceAll(instances, 1), std::exception);
        EXPECT_EQ(scene.Snapshot(), old);
    };
    auto bad = description;
    bad.locals.pop_back();
    check(bad);
    bad = description;
    bad.locals[0].node_id = "missing";
    check(bad);
    bad = description;
    bad.locals[0].node_id = bad.locals[1].node_id;
    check(bad);
    bad = description;
    bad.locals[0].local[3] = 0.01;
    check(bad);
    const std::array duplicate{description, description};
    EXPECT_THROW(scene.ReplaceAll(duplicate, 1), std::exception);
    EXPECT_EQ(scene.Snapshot(), old);
    const std::array valid{description};
    EXPECT_THROW(scene.ReplaceAll(valid, 0), std::exception);
    EXPECT_EQ(scene.Snapshot(), old);
}
