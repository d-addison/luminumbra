#include "ViewportServe.h"
#include "PreviewBuildIdentity.h"
#include "ViewportProtocol.h"
#include "authoring/PrefabDigest.h"
#include "core/Environment.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <luminumbra/rendering/StaticRenderer.h>
#include <map>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Luminumbra::Viewport {
namespace {
using namespace Rendering;
namespace fs = std::filesystem;
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
fs::path Path(const std::string& value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
void SafePath(const fs::path& path) {
    Require(path.is_absolute(), "Viewport startup paths must be absolute");
    auto prefix = path.root_path();
    for (const auto& part : path.relative_path()) {
        Require(part != "..", "Viewport startup parent traversal");
        prefix /= part;
        Require(!fs::is_symlink(fs::symlink_status(prefix)), "Viewport symlink input");
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(prefix.c_str());
        Require(attributes == INVALID_FILE_ATTRIBUTES ||
                    !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Viewport reparse input");
#endif
    }
}
std::vector<std::uint8_t> Read(const fs::path& path, std::uintmax_t limit = 256ull * 1024 * 1024) {
    SafePath(path);
    Require(fs::is_regular_file(path) && fs::file_size(path) <= limit,
            "Viewport missing or oversized file");
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fs::file_size(path)));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(input.good() && input.peek() == std::char_traits<char>::eof(),
            "Viewport input changed while reading");
    return bytes;
}
std::string Hash(std::span<const std::uint8_t> bytes) {
    return Authoring::Detail::Sha256(bytes);
}
void WriteJson(const fs::path& path, const Json& value) {
    Require(!fs::exists(path), "Viewport evidence already exists");
    std::ofstream output(path, std::ios::binary);
    output << value.dump(2) << '\n';
    output.close();
    Require(output.good(), "Viewport evidence write failed");
}
std::uint64_t Pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
fs::path Executable() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Require(count && count < path.size(), "Viewport executable identity");
    path.resize(count);
    return path;
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}
void ClearEnvironment(const char* name) {
#ifdef _WIN32
    Require(_putenv_s(name, "") == 0, "Cannot clear viewport child environment");
#else
    Require(unsetenv(name) == 0, "Cannot clear viewport child environment");
#endif
}
// Preserve the inherited binary pipe separately, then route every existing engine
// stdout/printf logger to stderr. No renderer log bytes may enter the wire stream.
class PipeOutput {
public:
    PipeOutput() {
        std::cout.flush();
        std::fflush(stdout);
#ifdef _WIN32
        Require(_setmode(_fileno(stdin), _O_BINARY) != -1, "Viewport binary input mode");
        const int copy = _dup(_fileno(stdout));
        Require(copy >= 0, "Viewport output duplicate");
        if (_setmode(copy, _O_BINARY) == -1 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
            _close(copy);
            throw std::runtime_error("Viewport output isolation");
        }
        stream = _fdopen(copy, "wb");
        if (!stream)
            _close(copy);
#else
        const int copy = dup(fileno(stdout));
        Require(copy >= 0, "Viewport output duplicate");
        if (dup2(fileno(stderr), fileno(stdout)) < 0) {
            close(copy);
            throw std::runtime_error("Viewport output isolation");
        }
        stream = fdopen(copy, "wb");
        if (!stream)
            close(copy);
#endif
        Require(stream, "Viewport binary output stream");
    }
    ~PipeOutput() {
        if (stream)
            std::fclose(stream);
    }
    void Write(std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
            const auto count = std::fwrite(bytes.data(), 1, bytes.size(), stream);
            Require(count > 0, "Viewport pipe write");
            bytes = bytes.subspan(count);
        }
        Require(std::fflush(stream) == 0, "Viewport pipe flush");
    }

private:
    std::FILE* stream = nullptr;
};
// Armed during startup, each complete record operation and shutdown; idle waiting
// for the first byte is allowed. The broker independently bounds stalled writes.
class Deadline {
public:
    explicit Deadline(const fs::path& directory)
        : worker([this, directory] {
            std::unique_lock lock(mutex);
            while (!done) {
                if (!armed) {
                    wake.wait(lock, [this] { return done || armed; });
                    continue;
                }
                const auto until = deadline;
                if (wake.wait_until(
                        lock, until, [this, until] { return done || !armed || deadline != until; }))
                    continue;
                lock.unlock();
                try {
                    WriteJson(directory / "hang.json",
                              {{"schema", "luminumbra.viewport.hang.v1"},
                               {"pid", Pid()},
                               {"deadline_seconds", 60}});
                } catch (...) {}
                std::cerr << "viewport watchdog deadline exceeded\n" << std::flush;
                std::_Exit(124);
            }
        }) {
        std::cerr << "viewport watchdog armed: 60 seconds per active operation\n";
    }
    ~Deadline() {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        wake.notify_one();
        worker.join();
    }
    void Arm(bool value) {
        {
            std::lock_guard lock(mutex);
            armed = value;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
        wake.notify_one();
    }

private:
    std::mutex mutex;
    std::condition_variable wake;
    bool done = false, armed = true;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::thread worker;
};
Record Next(const Key& key, Deadline& deadline) {
    deadline.Arm(false);
    const int first = std::fgetc(stdin);
    Require(first != EOF, "Viewport disconnected without authenticated stop");
    deadline.Arm(true);
    std::vector<std::uint8_t> bytes(16);
    bytes[0] = static_cast<std::uint8_t>(first);
    auto read = [&](std::size_t begin) {
        while (begin < bytes.size()) {
            const auto count = std::fread(bytes.data() + begin, 1, bytes.size() - begin, stdin);
            Require(count > 0, "Viewport truncated pipe record");
            begin += count;
        }
    };
    read(1);
    bytes.resize(RecordSize(bytes, true));
    read(16);
    return Decode(bytes, key);
}
RenderView Camera(const Json& state) {
    RenderViewDescription view;
    view.view = state.at("view").get<ViewMatrix>();
    view.projection = state.at("projection").get<ViewMatrix>();
    view.width = state.at("width");
    view.height = state.at("height");
    // Wire revisions admit zero. Internal validated camera identity is offset by
    // one; the authenticated response echoes the original wire value unchanged.
    view.revision = state.at("camera_revision").get<std::uint64_t>() + 1;
    view.near_plane = state.at("near_plane");
    view.far_plane = state.at("far_plane");
    return RenderView::Validate(view);
}
std::vector<StaticInstanceDescription> Instances(const Json& state,
                                                 std::shared_ptr<const StaticPrefab> prefab) {
    std::map<std::string, StaticInstanceDescription> instances;
    for (const auto& local : state.at("locals")) {
        const auto id = local.at("instance_id").get<std::string>();
        auto& instance = instances[id];
        instance.instance_id = id;
        instance.prefab = prefab;
        instance.locals.push_back({local.at("node_id"), local.at("matrix").get<StaticMatrix4>()});
    }
    std::vector<StaticInstanceDescription> result;
    for (auto& [id, instance] : instances) {
        (void)id;
        result.push_back(std::move(instance));
    }
    return result;
}
Json FrameReceipt(const StaticFrame& frame, const Json& header, const std::string& planes) {
    return {{"wire_header", header},
            {"renderer_sequence", frame.sequence},
            {"internal_scene_revision", frame.scene_revision},
            {"actual_view", frame.actual_view},
            {"actual_projection", frame.actual_projection},
            {"planes_sha256", planes},
            {"draws", frame.draw_count},
            {"indices", frame.index_count},
            {"mesh_uploads", frame.uploaded_meshes},
            {"texture_uploads", frame.uploaded_textures},
            {"updated_instances", frame.updated_instances},
            {"covered_pixels", std::count(frame.coverage8.begin(), frame.coverage8.end(), 1)},
            {"synchronous_render_readback_ms", frame.synchronous_render_readback_ms},
            {"gpu",
             {{"vendor", frame.vendor}, {"renderer", frame.renderer}, {"version", frame.version}}}};
}
} // namespace
int Serve(int argc, char** argv) {
    fs::path output;
    bool owns = false;
    try {
        fs::path resources;
        bool software = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--software-only") {
                software = true;
                continue;
            }
            Require(i + 1 < argc, "Viewport option value missing");
            if (argument == "--output" && output.empty())
                output = Path(argv[++i]);
            else if (argument == "--resource-root" && resources.empty())
                resources = Path(argv[++i]);
            else
                throw std::invalid_argument("Unknown or duplicate viewport option");
        }
        const auto key_text = Core::ReadEnvironment("LUMINUMBRA_VIEWPORT_KEY");
        const auto project_text = Core::ReadEnvironment("LUMINUMBRA_VIEWPORT_PROJECT");
        ClearEnvironment("LUMINUMBRA_VIEWPORT_KEY");
        ClearEnvironment("LUMINUMBRA_VIEWPORT_PROJECT");
        Require(key_text && project_text, "Viewport trusted startup environment missing");
        const auto key = DecodeKey(*key_text);
        const auto project = Path(*project_text);
        SafePath(project);
        Require(fs::is_directory(project), "Viewport project directory missing");
        Require(!output.empty(), "Viewport fresh evidence directory required");
        SafePath(output);
        Require(fs::create_directory(output), "Viewport evidence directory must be fresh");
        owns = true;
        PipeOutput pipe;
        Deadline deadline(output);
        const auto executable = Executable(), module = Path(StaticRenderer::ModulePath());
        if (resources.empty())
            resources = executable.parent_path().parent_path() / "share/luminumbra-static";
        SafePath(resources);
        fs::current_path(output); // Any inherited engine log stays inside this owned session.
        std::map<fs::path, std::string> inputs;
        for (const auto& path : {executable, module, resources / "source-inputs.txt"})
            inputs[path] = Hash(Read(path));
        Require(inputs.at(resources / "source-inputs.txt") == kPreviewSourceDigest,
                "Viewport installed source input digest");
        Json receipt{{"schema", "luminumbra.viewport.session.v1"},
                     {"pid", Pid()},
                     {"source_commit", kPreviewCommit},
                     {"source_dirty", kPreviewDirty},
                     {"source_input_sha256", kPreviewSourceDigest},
                     {"executable_sha256", inputs.at(executable)},
                     {"module_sha256", inputs.at(module)},
                     {"resources", Json::object()},
                     {"watchdog_seconds", 60},
                     {"visual_approved", false},
                     {"frame_count", 0},
                     {"mesh_uploads_total", 0},
                     {"texture_uploads_total", 0},
                     {"generation_loads", 0},
                     {"record_chain_sha256", std::string(64, '0')}};
        for (const char* name : {"static_asset.vert",
                                 "static_asset.frag",
                                 "lighting_pass.vert",
                                 "lighting_pass.frag",
                                 "config_constants.gen.glsl"}) {
            const auto path = resources / "res/shaders" / name;
            inputs[path] = Hash(Read(path, 2 * 1024 * 1024));
            receipt["resources"][name] = inputs.at(path);
        }
        StateGate gate;
        Json previous;
        StaticScene scene;
        std::shared_ptr<const StaticPrefab> prefab;
        {
            StaticRenderer renderer(Utf8(resources), software);
            while (true) {
                const auto request = Next(key, deadline);
                gate.Accept(request.header);
                receipt["session"] = request.header.at("session");
                receipt["last_sequence"] = request.header.at("sequence");
                if (request.header.at("kind") == "stop")
                    break;
                const auto& state = request.header.at("state");
                const auto camera = Camera(state);
                const bool generation_changed =
                    previous.is_null() || state.at("generation_id") != previous.at("generation_id");
                auto candidate = prefab;
                if (generation_changed) {
                    candidate = StaticPrefab::Load(
                        Utf8(project), state.at("generation_id"), state.at("manifest_sha256"));
                    receipt["generation_loads"] =
                        receipt.at("generation_loads").get<std::uint64_t>() + 1;
                }
                if (generation_changed || state.at("locals") != previous.at("locals")) {
                    scene.ReplaceAll(Instances(state, candidate), scene.Snapshot()->revision);
                    Require(scene.Snapshot()->draws.size() <= 1024, "Viewport draw receipt bound");
                    prefab = std::move(candidate);
                }
                const auto frame = renderer.Render(camera, scene.Snapshot());
                static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);
                std::vector<std::uint8_t> payload = frame.rgba8;
                const auto* depth = reinterpret_cast<const std::uint8_t*>(frame.depth32f.data());
                payload.insert(payload.end(), depth, depth + frame.depth32f.size() * sizeof(float));
                payload.insert(payload.end(), frame.coverage8.begin(), frame.coverage8.end());
                auto header = request.header;
                header["kind"] = "frame";
                header["planes_sha256"] = Hash(payload);
                const auto record = Encode({header, std::move(payload)}, key);
                pipe.Write(record);
                receipt["last_frame"] = FrameReceipt(frame, header, header.at("planes_sha256"));
                if (!receipt.contains("recent_frames"))
                    receipt["recent_frames"] = Json::array();
                auto& recent = receipt["recent_frames"];
                if (recent.size() == 8)
                    recent.erase(recent.begin());
                recent.push_back(receipt.at("last_frame"));
                auto chain = receipt.at("record_chain_sha256").get<std::string>() + Hash(record);
                receipt["record_chain_sha256"] =
                    Hash({reinterpret_cast<const std::uint8_t*>(chain.data()), chain.size()});
                receipt["frame_count"] = receipt.at("frame_count").get<std::uint64_t>() + 1;
                receipt["mesh_uploads_total"] =
                    receipt.at("mesh_uploads_total").get<std::uint64_t>() + frame.uploaded_meshes;
                receipt["texture_uploads_total"] =
                    receipt.at("texture_uploads_total").get<std::uint64_t>() +
                    frame.uploaded_textures;
                previous = state;
            }
            deadline.Arm(true);
        }
        for (const auto& [path, hash] : inputs)
            Require(Hash(Read(path)) == hash, "Viewport installed input changed during session");
        receipt["shutdown_complete"] = true;
        receipt["status"] = "complete";
        WriteJson(output / "session.json", receipt);
        std::cerr << "viewport authenticated stop; renderer shutdown complete\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "viewport refused: " << error.what() << '\n';
        if (owns)
            try {
                WriteJson(
                    output / "failure.json",
                    {{"status", "refused"}, {"error", error.what()}, {"visual_approved", false}});
            } catch (...) {}
        return 1;
    }
}
} // namespace Luminumbra::Viewport
