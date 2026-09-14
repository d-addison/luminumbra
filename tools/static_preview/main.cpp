#include "PreviewBuildIdentity.h"
#include "ViewportServe.h"
#include "authoring/PrefabDigest.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <luminumbra/rendering/StaticRenderer.h>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace Luminumbra::Rendering;
using Json = nlohmann::json;
namespace fs = std::filesystem;
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
fs::path Path(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string Utf8(const fs::path& value) {
    const auto text = value.u8string();
    return {text.begin(), text.end()};
}
std::vector<std::uint8_t> Read(const fs::path& path, std::uintmax_t limit = 256ull * 1024 * 1024) {
    Require(fs::is_regular_file(path) && fs::file_size(path) <= limit,
            "Missing or oversized input file");
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(fs::file_size(path)));
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Require(input.good() && input.peek() == std::char_traits<char>::eof(),
            "Input changed or could not be read completely");
    return bytes;
}
std::string Hash(std::span<const std::uint8_t> bytes) {
    return Luminumbra::Authoring::Detail::Sha256(bytes);
}
void Write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    Require(!fs::exists(path), "Capture member already exists");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    Require(output.good(), "Capture member write failed");
}
void WriteJson(const fs::path& path, const Json& value) {
    const auto text = value.dump(2) + "\n";
    Write(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
void Fields(const Json& value, std::initializer_list<const char*> allowed) {
    Require(value.is_object(), "Expected a JSON object");
    const std::set<std::string> fields(allowed.begin(), allowed.end());
    for (const auto& [key, item] : value.items()) {
        (void)item;
        Require(fields.contains(key), "Unknown request field");
    }
}
fs::path Executable() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), path.size());
    Require(n && n < path.size(), "Cannot resolve running executable");
    path.resize(n);
    return fs::path(path);
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}
std::uint64_t Pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
class Watchdog {
public:
    explicit Watchdog(const fs::path& output)
        : worker([this, output] {
            std::unique_lock lock(mutex);
            if (!wake.wait_for(lock, std::chrono::seconds(60), [this] { return done; })) {
                try {
                    WriteJson(output / "hang.json",
                              {{"schema", "luminumbra.static_preview.hang.v1"},
                               {"pid", Pid()},
                               {"deadline_seconds", 60}});
                } catch (...) {}
                std::cerr << "static-preview watchdog deadline exceeded\n" << std::flush;
                std::_Exit(124);
            }
        }) {
        std::cerr << "static-preview watchdog armed: 60 seconds\n";
    }
    ~Watchdog() {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        wake.notify_one();
        worker.join();
    }

private:
    std::mutex mutex;
    std::condition_variable wake;
    bool done = false;
    std::thread worker;
};
Json Parse(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 32, "Request nesting exceeds limit");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(keys.back().insert(value.get<std::string>()).second,
                    "Duplicate JSON request key");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--serve")
        return Luminumbra::Viewport::Serve(argc - 1, argv + 1);
    fs::path output;
    bool owns_output = false;
    try {
        fs::path request_path, resources;
        bool software = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "luminumbra_preview_capture --request FILE --output FRESH_DIR "
                             "[--resource-root DIR] [--software-only]\n"
                             "luminumbra_preview_capture --serve --output FRESH_ABSOLUTE_DIR "
                             "[--resource-root DIR] [--software-only]\n";
                return 0;
            }
            if (arg == "--software-only") {
                software = true;
                continue;
            }
            Require(i + 1 < argc, "Missing option value");
            if (arg == "--request")
                request_path = Path(argv[++i]);
            else if (arg == "--output")
                output = Path(argv[++i]);
            else if (arg == "--resource-root")
                resources = Path(argv[++i]);
            else
                throw std::invalid_argument("Unknown capture option");
        }
        Require(!request_path.empty() && !output.empty() && !fs::exists(output),
                "Request and fresh output directory are required");
        const auto executable = Executable();
        if (resources.empty())
            resources = executable.parent_path().parent_path() / "share/luminumbra-static";
        const auto bytes = Read(request_path, 1024 * 1024);
        const auto request = Parse(bytes);
        Fields(request, {"schema", "prefabs", "instances", "frames"});
        Require(request.at("schema") == "luminumbra.static_preview.request.v1",
                "Unknown static capture request schema");
        Require(request.at("prefabs").is_array() && !request.at("prefabs").empty() &&
                    request.at("prefabs").size() <= 8,
                "Expected 1..8 pinned prefabs");
        Require(request.at("instances").is_array() && !request.at("instances").empty() &&
                    request.at("instances").size() <= 64,
                "Expected 1..64 instances");
        Require(request.at("frames").is_array() && !request.at("frames").empty() &&
                    request.at("frames").size() <= 8,
                "Expected 1..8 requested frames");
        Require(fs::create_directory(output),
                "Capture output must be a fresh child of an existing directory");
        owns_output = true;
        Watchdog watchdog(output);
        const auto module = Path(StaticRenderer::ModulePath());
        Json receipt{{"schema", "luminumbra.static_preview.capture.v1"},
                     {"pid", Pid()},
                     {"source_commit", kPreviewCommit},
                     {"source_dirty", kPreviewDirty},
                     {"source_input_sha256", kPreviewSourceDigest},
                     {"executable_sha256", Hash(Read(executable))},
                     {"module_sha256", Hash(Read(module))},
                     {"request_sha256", Hash(bytes)},
                     {"watchdog_seconds", 60},
                     {"visual_approved", false},
                     {"frames", Json::array()},
                     {"generations", Json::object()},
                     {"resources", Json::object()},
                     {"profile", "static-perspective-opaque-mask-v1"}};
        for (const char* name : {"static_asset.vert",
                                 "static_asset.frag",
                                 "lighting_pass.vert",
                                 "lighting_pass.frag",
                                 "config_constants.gen.glsl"})
            receipt["resources"][name] =
                Hash(Read(resources / "res/shaders" / name, 2 * 1024 * 1024));
        std::map<std::string, std::shared_ptr<const StaticPrefab>> prefabs;
        for (const auto& item : request.at("prefabs")) {
            Fields(item, {"id", "project_root", "generation_id", "manifest_sha256"});
            const auto id = item.at("id").get<std::string>();
            Require(!id.empty() && id.size() <= 128 && !prefabs.contains(id),
                    "Invalid or duplicate prefab request ID");
            prefabs.emplace(id,
                            StaticPrefab::Load(item.at("project_root"),
                                               item.at("generation_id"),
                                               item.at("manifest_sha256")));
            receipt["generations"][id] = {{"generation_id", prefabs.at(id)->generation_id()},
                                          {"manifest_sha256", prefabs.at(id)->manifest_sha256()}};
        }
        StaticScene scene;
        std::set<std::string> instance_ids;
        for (const auto& item : request.at("instances")) {
            Fields(item, {"id", "prefab", "placement"});
            Require(instance_ids.insert(item.at("id").get<std::string>()).second,
                    "Duplicate instance request ID");
            Require(item.at("placement").is_array() && item.at("placement").size() == 16,
                    "Placement matrix must contain 16 numbers");
            scene.Replace(item.at("id"),
                          prefabs.at(item.at("prefab").get<std::string>()),
                          item.at("placement").get<StaticMatrix4>(),
                          scene.Snapshot()->revision);
        }
        Require(scene.Snapshot()->draws.size() <= 1024,
                "Capture receipt profile supports at most 1024 draws");
        std::uint64_t previous_camera_revision = 0;
        {
            StaticRenderer renderer(Utf8(resources), software);
            for (const auto& item : request.at("frames")) {
                Fields(item, {"camera", "updates"});
                if (item.contains("updates")) {
                    Require(item.at("updates").is_array() && item.at("updates").size() <= 65536,
                            "Invalid matrix update batch");
                    std::vector<StaticLocalMatrixUpdate> updates;
                    for (const auto& update : item.at("updates")) {
                        Fields(update, {"instance_id", "node_id", "local"});
                        Require(update.at("local").is_array() && update.at("local").size() == 16,
                                "Local matrix must contain 16 numbers");
                        updates.push_back({update.at("instance_id"),
                                           update.at("node_id"),
                                           update.at("local").get<StaticMatrix4>()});
                    }
                    if (!updates.empty())
                        scene.UpdateLocalMatrices(updates, scene.Snapshot()->revision);
                }
                const auto& camera = item.at("camera");
                Fields(camera,
                       {"view",
                        "projection",
                        "width",
                        "height",
                        "revision",
                        "near_plane",
                        "far_plane"});
                Require(camera.at("view").is_array() && camera.at("view").size() == 16 &&
                            camera.at("projection").is_array() &&
                            camera.at("projection").size() == 16,
                        "Camera matrices must contain 16 numbers");
                RenderViewDescription description;
                description.view = camera.at("view").get<ViewMatrix>();
                description.projection = camera.at("projection").get<ViewMatrix>();
                Require(camera.at("width").is_number_unsigned() &&
                            camera.at("height").is_number_unsigned() &&
                            camera.at("revision").is_number_unsigned(),
                        "Camera extents/revision must be unsigned integers");
                const auto width = camera.at("width").get<std::uint64_t>(),
                           height = camera.at("height").get<std::uint64_t>();
                Require(width > 0 && height > 0 && width <= 4096 && height <= 4096,
                        "Camera extent exceeds profile bounds");
                description.width = static_cast<std::uint32_t>(width);
                description.height = static_cast<std::uint32_t>(height);
                description.revision = camera.at("revision");
                description.near_plane = camera.at("near_plane");
                description.far_plane = camera.at("far_plane");
                Require(description.revision > previous_camera_revision,
                        "Capture camera revisions must increase");
                const auto frame =
                    renderer.Render(RenderView::Validate(description), scene.Snapshot());
                previous_camera_revision = description.revision;
                const auto directory = output / ("frame-" + std::to_string(frame.sequence));
                fs::create_directory(directory);
                const std::span<const std::uint8_t> depth(
                    reinterpret_cast<const std::uint8_t*>(frame.depth32f.data()),
                    frame.depth32f.size() * sizeof(float));
                static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);
                Write(directory / "color.rgba8", frame.rgba8);
                Write(directory / "depth.f32le", depth);
                Write(directory / "coverage.u8", frame.coverage8);
                Json record{
                    {"sequence", frame.sequence},
                    {"scene_revision", frame.scene_revision},
                    {"camera", camera},
                    {"actual_view", frame.actual_view},
                    {"actual_projection", frame.actual_projection},
                    {"origin", "bottom-left"},
                    {"deferred_position_format", "RGB32F"},
                    {"color",
                     {{"file", "color.rgba8"},
                      {"bytes", frame.rgba8.size()},
                      {"sha256", Hash(frame.rgba8)},
                      {"encoding", "production-inspection-power-2.2"},
                      {"alpha", "straight-coverage"}}},
                    {"depth",
                     {{"file", "depth.f32le"},
                      {"bytes", depth.size()},
                      {"sha256", Hash(depth)},
                      {"range", {0, 1}},
                      {"near", 1},
                      {"clear", 0},
                      {"projection", "RH-ZO-reversed-finite"}}},
                    {"coverage",
                     {{"file", "coverage.u8"},
                      {"bytes", frame.coverage8.size()},
                      {"sha256", Hash(frame.coverage8)},
                      {"covered_pixels",
                       std::count(frame.coverage8.begin(), frame.coverage8.end(), 1)}}},
                    {"draws", frame.draw_count},
                    {"indices", frame.index_count},
                    {"mesh_uploads", frame.uploaded_meshes},
                    {"texture_uploads", frame.uploaded_textures},
                    {"updated_instances", frame.updated_instances},
                    {"synchronous_render_readback_ms", frame.synchronous_render_readback_ms},
                    {"gpu",
                     {{"vendor", frame.vendor},
                      {"renderer", frame.renderer},
                      {"version", frame.version}}}};
                record["instances"] = Json::array();
                for (const auto& draw : scene.Snapshot()->draws)
                    record["instances"].push_back(
                        {{"instance_id", draw.key.instance_id},
                         {"node_id", draw.key.node_id},
                         {"primitive_index", draw.key.primitive_index},
                         {"material_id", draw.material_id},
                         {"model", draw.model},
                         {"normal", draw.normal},
                         {"reverse_front_face", draw.reverse_front_face},
                         {"generation_id", draw.generation->generation_id()},
                         {"manifest_sha256", draw.generation->manifest_sha256()}});
                WriteJson(directory / "frame.json", record);
                receipt["frames"].push_back(
                    {{"directory", directory.filename().string()},
                     {"receipt_sha256", Hash(Read(directory / "frame.json", 2 * 1024 * 1024))}});
            }
        }
        for (const auto& [name, hash] : receipt["resources"].items())
            Require(Hash(Read(resources / "res/shaders" / name, 2 * 1024 * 1024)) ==
                        hash.get<std::string>(),
                    "Shader resource changed during capture");
        Require(Hash(Read(executable)) == receipt.at("executable_sha256").get<std::string>(),
                "Executable changed during capture");
        Require(Hash(Read(module)) == receipt.at("module_sha256").get<std::string>(),
                "Renderer module changed during capture");
        receipt["shutdown_complete"] = true;
        receipt["status"] = "complete";
        WriteJson(output / "capture.json", receipt);
        std::cout << receipt.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "static-preview refused: " << error.what() << '\n';
        if (owns_output && fs::is_directory(output) && !fs::exists(output / "failure.json")) {
            try {
                WriteJson(
                    output / "failure.json",
                    {{"status", "refused"}, {"error", error.what()}, {"visual_approved", false}});
            } catch (...) {}
        }
        return 1;
    }
}
