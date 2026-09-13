#include <luminumbra/rendering/RenderView.h>
#include <luminumbra/rendering/StaticRenderer.h>
#include <luminumbra/rendering/StaticScene.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Luminumbra::Rendering;

namespace {
unsigned checks = 0;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

void Near(double actual, double expected, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-5, message);
}

template <class Operation>
void Refuses(Operation operation, const char* message) {
    bool refused = false;
    try {
        operation();
    } catch (const std::exception&) {
        refused = true;
    }
    Check(refused, message);
}

StaticMatrix4 Translation(double x, double y, double z) {
    auto matrix = kStaticIdentity;
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    return matrix;
}

std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

const StaticDraw& Draw(const StaticDrawSnapshot& snapshot, const std::string& node) {
    for (const auto& draw : snapshot.draws) {
        if (draw.key.node_id == node && draw.key.primitive_index == 0) return draw;
    }
    throw std::runtime_error("fixture draw missing: " + node);
}

void SceneChecks(const char* project, const char* generation, const char* digest) {
    auto prefab = StaticPrefab::Load(project, generation, digest);
    Check(prefab->generation_id() == generation && prefab->manifest_sha256() == digest,
          "loaded generation identity differs from explicit pin");
    Refuses([&] { StaticPrefab::Load(project, generation, std::string(64, '0')); },
            "incorrect manifest digest accepted");
    std::shared_ptr<const StaticDrawSnapshot> held;
    {
        StaticScene scene;
        const auto empty = scene.Snapshot();
        Check(empty->revision == 0 && empty->draws.empty(), "new scene is not empty revision zero");
        Check(empty == scene.Snapshot(), "unchanged snapshot identity is unstable");
        scene.Replace("consumer.instance", prefab, Translation(2, 3, 4), 0);
        held = scene.Snapshot();
        Check(held->revision == 1 && held->draws.size() == 4, "fixture publication differs");
        const auto& first = Draw(*held, "fixture.first");
        const auto& second = Draw(*held, "fixture.second");
        Check(first.key.instance_id == "consumer.instance" && first.generation == prefab,
              "draw lost instance or generation identity");
        Near(first.model[12], 13, "parent and instance translations were not composed");
        Near(first.model[13], 5, "composed Y differs");
        Near(first.model[14], 7, "composed Z differs");
        Near(second.model[12], 12, "sibling translation differs");
        Check(!first.reverse_front_face && second.reverse_front_face, "mirror winding differs");

        StaticLocalMatrixUpdate root{"consumer.instance", "fixture.root", Translation(20, 2, 3)};
        scene.UpdateLocalMatrices(std::span(&root, 1), 1);
        const auto updated = scene.Snapshot();
        Check(updated != held && updated->revision == 2, "update did not publish one revision");
        const auto& moved = Draw(*updated, "fixture.first");
        Near(moved.model[12], 23, "parent edit did not move child");
        Near(Draw(*updated, "fixture.second").model[12], 22, "parent edit did not move sibling");
        Near(first.model[12], 13, "held snapshot changed after edit");
        Check(moved.key == first.key && moved.mesh == first.mesh && moved.material == first.material,
              "transform edit rebuilt immutable resources or changed draw identity");

        auto unchanged_refusal = [&](auto operation, const char* message) {
            Refuses(operation, message);
            Check(scene.Snapshot() == updated, "refused mutation changed snapshot identity");
        };
        unchanged_refusal([&] { scene.UpdateLocalMatrices(std::span(&root, 1), 1); },
                          "stale revision accepted");
        auto singular = root;
        singular.local[0] = 0;
        unchanged_refusal([&] { scene.UpdateLocalMatrices(std::span(&singular, 1), 2); },
                          "singular transform accepted");
        auto unknown = root;
        unknown.node_id = "fixture.absent";
        unchanged_refusal([&] { scene.UpdateLocalMatrices(std::span(&unknown, 1), 2); },
                          "unknown node accepted");
        const std::array duplicates{root, root};
        unchanged_refusal([&] { scene.UpdateLocalMatrices(duplicates, 2); },
                          "duplicate node update accepted");
        unchanged_refusal([&] { scene.Remove("absent.instance", 2); }, "unknown instance removed");
        unchanged_refusal([&] { scene.Replace("consumer.instance", nullptr, kStaticIdentity, 2); },
                          "null generation accepted");
        StaticInstanceDescription incomplete{"consumer.instance", prefab, kStaticIdentity, {}};
        unchanged_refusal([&] { scene.ReplaceAll(std::span(&incomplete, 1), 2); },
                          "replacement with incomplete descriptor membership accepted");
        scene.UpdateLocalMatrices({}, 2);
        Check(scene.Snapshot() == updated, "empty transform update published a revision");
        scene.Remove("consumer.instance", 2);
        Check(scene.Snapshot()->revision == 3 && scene.Snapshot()->draws.empty(),
              "instance removal did not publish empty revision three");
    }
    prefab.reset();
    Check(held->revision == 1 && held->draws.size() == 4,
          "held snapshot lost lifetime after scene destruction");
    const auto& surviving = Draw(*held, "fixture.first");
    Check(!surviving.mesh->vertices.empty() && !surviving.mesh->indices.empty() &&
              surviving.generation->generation_id() == generation,
          "held snapshot resources did not survive scene destruction");
}

void CameraChecks() {
    RenderViewDescription description;
    description.width = 640;
    description.height = 480;
    description.revision = 17;
    description.near_plane = .1;
    description.far_plane = 100;
    description.view = Translation(-2, -3, -4);
    auto& projection = description.projection;
    const double near = description.near_plane, far = description.far_plane;
    projection[0] = 1.5;
    projection[5] = 2;
    projection[10] = near / (far - near);
    projection[11] = -1;
    projection[14] = far * near / (far - near);
    const auto perspective = RenderView::Validate(description);
    Check(!perspective.orthographic() && perspective.description().revision == 17,
          "perspective type or revision differs");
    Near(perspective.eye()[0], 2, "eye X differs");
    Near(perspective.eye()[1], 3, "eye Y differs");
    Near(perspective.eye()[2], 4, "eye Z differs");

    auto common = [&](const RenderView& view) {
        Check(view.description().view == description.view &&
                  view.description().projection == description.projection,
              "accepted camera description was reconstructed");
        for (std::size_t index = 0; index < 16; ++index) {
            Check(view.view()[index] == static_cast<float>(description.view[index]) &&
                      view.projection()[index] == static_cast<float>(description.projection[index]),
                  "accepted camera float matrices differ from supplied matrices");
        }
        auto depth = [&](double distance) {
            const auto& p = view.projection();
            return (-distance * p[10] + p[14]) / (-distance * p[11] + p[15]);
        };
        Near(depth(near), 1, "near depth is not reversed-Z one");
        Near(depth(far), 0, "far depth is not reversed-Z zero");
        for (const auto& plane : view.frustum_planes()) {
            Near(std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]),
                 1, "frustum plane is not normalized");
            Check(plane[0] * 2 + plane[1] * 3 - plane[2] * 6 + plane[3] > 0,
                  "interior world point rejected by supplied-camera frustum");
        }
    };
    common(perspective);
    auto shifted = description;
    shifted.projection[8] = .1;
    Refuses([&] { RenderView::Validate(shifted); }, "shifted perspective accepted");

    projection = {};
    projection[0] = .25;
    projection[5] = 1. / 3;
    projection[10] = 1 / (far - near);
    projection[12] = -.5;
    projection[13] = -1. / 3;
    projection[14] = far / (far - near);
    projection[15] = 1;
    const auto orthographic = RenderView::Validate(description);
    Check(orthographic.orthographic(), "off-center orthographic camera refused or misclassified");
    common(orthographic);
    Near(-(near + far) * .5 * orthographic.projection()[10] + orthographic.projection()[14],
         .5, "orthographic depth is not linear");
    auto invalid = description;
    invalid.projection[11] = -1;
    Refuses([&] { RenderView::Validate(invalid); }, "hybrid projection accepted");
    invalid = description;
    invalid.view[0] = 2;
    Refuses([&] { RenderView::Validate(invalid); }, "non-rigid view accepted");
    invalid = description;
    invalid.width = 0;
    Refuses([&] { RenderView::Validate(invalid); }, "empty camera extent accepted");
    invalid = description;
    invalid.projection[10] = -1 / (far - near);
    invalid.projection[14] = -near / (far - near);
    Refuses([&] { RenderView::Validate(invalid); }, "forward-Z projection accepted");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::runtime_error("usage: installed_consumer PROJECT GENERATION MANIFEST_SHA256 EXPECTED_MODULE");
        }
        // This resolves the actual shared library without constructing a renderer or GPU context.
        const auto module = StaticRenderer::ModulePath();
        Check(std::filesystem::equivalent(Utf8Path(module), Utf8Path(argv[4])),
              "loaded module is not the explicitly expected installed library");
        std::cout << "loaded_module=" << module << '\n';
        SceneChecks(argv[1], argv[2], argv[3]);
        CameraChecks();
        std::cout << "passed=true checks=" << checks << " gpu_constructed=false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "passed=false checks=" << checks << " error=" << error.what() << '\n';
        return 1;
    }
}
