#include "luminumbra_client/app/RenderBenchmarkReport.h"
#include "luminumbra_client/app/RenderConfiguration.h"
#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_client/app/RenderWorkload.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldgenFileReader.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using namespace Luminumbra::Client::Measurement;
using luminumbra::core::SystemConfig;
using nlohmann::json;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("render configuration assertion failed at line " +
                                 std::to_string(location.line()));
}

void configuration() {
    std::ifstream input(std::string(LUMINUMBRA_SOURCE_ROOT) +
                        "/src/luminumbra_common/core/ConfigSchema.json");
    const auto schema = json::parse(input);
    const auto defaults = render_systems(SystemConfig::Defaults());
    json expected = json::object();
    for (const auto& key : schema.at("keys")) {
        if (key.at("section") != "render")
            continue;
        const auto name = key.at("json_name").get<std::string>();
        expected[name] = {{"enabled", true}, {"params", json::object()}};
        for (const auto& param : schema.at("params")) {
            if (param.at("owner") != key.at("enum"))
                continue;
            auto value = param.at("default");
            if (value.is_array()) {
                for (auto& component : value)
                    component = static_cast<float>(component.get<double>());
            } else {
                value = static_cast<float>(value.get<double>());
            }
            expected[name]["params"][param.at("json_name").get<std::string>()] = value;
        }
    }
    require(defaults == expected);
    const auto identity = render_identity(defaults);
    for (const auto& [name, entry] : expected.items()) {
        // A default-file change has no CLI declaration, but changes identity.
        const json flags = {{"render", {{name, {{"enabled", false}}}}}};
        const auto changed = render_systems(SystemConfig::FromJsonString(flags.dump()));
        require(changed.at(name).at("enabled") == false);
        require(render_identity(changed) != identity);
        for (const auto& [param, value] : entry.at("params").items()) {
            auto replacement = value;
            if (replacement.is_array())
                replacement[0] = static_cast<float>(replacement[0].get<double>() + 0.125);
            else if (name == "far_world_ladder" && param == "cave_field")
                // This scalar is an exact enum code, not a continuous tuning value.
                replacement = value == 0 ? 1.0f : 0.0f;
            else
                replacement = static_cast<float>(replacement.get<double>() + 0.125);
            const json configured = {{"render", {{name, {{"params", {{param, replacement}}}}}}}};
            const auto resolved = render_systems(SystemConfig::FromJsonString(configured.dump()));
            require(resolved.at(name).at("params").at(param) == replacement);
            require(render_identity(resolved) != identity);
        }
    }
    // Shared Python/native oracle covers sorting, UTF-8, booleans, null,
    // unsigned integers, doubles and the sign bit of zero.
    const json oracle = {{"b", {true, false, nullptr, 1, 18446744073709551615ull, 1.5, -0.0}},
                         {"a", "snow \xe2\x98\x83"}};
    require(render_identity(oracle) == "fnv1a64:bd4e71012aed102e");
}

void graphics_capture_configuration() {
    luminumbra::core::GraphicsSettings settings;
    luminumbra::core::ResolvedGraphics resolved;
    std::string error;
    require(luminumbra::core::ResolveGraphics(settings, {}, resolved, error));
    const auto original = render_identity(graphics_configuration(settings, resolved));
    for (const auto preset : {luminumbra::core::GraphicsPreset::Low,
                              luminumbra::core::GraphicsPreset::Medium,
                              luminumbra::core::GraphicsPreset::High}) {
        settings.base_preset = preset;
        require(luminumbra::core::ResolveGraphics(settings, {}, resolved, error));
        const auto captured = graphics_configuration(settings, resolved);
        require(captured.at("base_preset").get<std::string>() ==
                luminumbra::core::GraphicsPresetName(preset));
        require(render_identity(captured) != original);
    }
    settings.overrides = {
        {"grass_density", 0.5}, {"grass_enabled", false}, {"ui_blur", std::string("off")}};
    const luminumbra::core::GraphicsOverrides session = {{"render_scale", 0.75}};
    require(luminumbra::core::ResolveGraphics(settings, session, resolved, error));
    const auto captured = graphics_configuration(settings, resolved);
    require(captured.at("saved_overrides").at("grass_density") == 0.5);
    require(captured.at("saved_overrides").at("grass_enabled") == false);
    require(captured.at("saved_overrides").at("ui_blur") == "off");
    require(captured.at("session_overrides").at("render_scale") == 0.75);
    require(!resolved.values.grass_enabled && resolved.values.render_scale == 0.75);
    auto unsupported = settings;
    unsupported.overrides["taau"] = false;
    const auto previous = resolved.values;
    require(!luminumbra::core::ResolveGraphics(unsupported, session, resolved, error));
    require(resolved.values == previous);
    require(render_identity(captured) != original);
    const json assets = {{"data/input.json", "unchanged"}};
    auto saved = assets;
    saved["inputs/user_settings.json"] = "created";
    require(content_after_settings_save(assets, saved) == saved);
    auto migrated = saved;
    migrated["inputs/user_settings.json"] = "migrated";
    require(content_after_settings_save(saved, migrated) == migrated);
    auto modified_asset = saved;
    modified_asset["data/input.json"] = "changed";
    auto added_asset = saved;
    added_asset["res/new.json"] = "added";
    auto removed_asset = saved;
    removed_asset.erase("data/input.json");
    auto unavailable_settings = saved;
    unavailable_settings["inputs/user_settings.json"] = nullptr;
    for (const json& changed : {json::object(),
                                assets,
                                modified_asset,
                                added_asset,
                                removed_asset,
                                unavailable_settings}) {
        bool refused = false;
        try {
            (void)content_after_settings_save(saved, changed);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused);
    }
}

void simulation_configuration() {
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(SystemConfig::Defaults());
    const auto original = session.SystemConfigIdentity();
    const auto identity = render_identity(json{{"simulation_identity", original}});
    const json water = {{"sim", {{"water_high_res", {{"enabled", false}}}}}};
    session.ApplySystemConfig(SystemConfig::FromJsonString(water.dump()));
    require(session.SystemConfigIdentity() != original);
    require(render_identity(json{{"simulation_identity", session.SystemConfigIdentity()}}) !=
            identity);
    session.ApplySystemConfig(SystemConfig::Defaults());
    require(session.SystemConfigIdentity() == original);
    auto ecology = session.GetEcologyTuning();
    ecology.energy_drain_per_second += 0.125f;
    session.SetEcologyTuning(ecology);
    require(session.SystemConfigIdentity() != original);
    require(render_identity(json{{"simulation_identity", session.SystemConfigIdentity()}}) !=
            identity);
}

class TemporaryContent {
public:
    TemporaryContent()
        : root_(std::filesystem::temp_directory_path() /
                ("render-content-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        require(std::filesystem::create_directory(root_));
        for (const auto* tree : Luminumbra::Filesystem::kCaptureContentTrees)
            require(std::filesystem::create_directories(root_ / tree));
    }
    ~TemporaryContent() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    const std::filesystem::path& root() const {
        return root_;
    }
    void write(const std::filesystem::path& relative, const std::string& bytes) const {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        require(output.good());
    }

private:
    const std::filesystem::path root_;
};

void content_configuration() {
    std::istringstream empty;
    std::istringstream hello("hello");
    require(content_digest(empty) == "fnv1a64:cbf29ce484222325");
    require(content_digest(hello) == "fnv1a64:a430d84680aabd0b");
    std::istringstream large(std::string(65536, 'a') + "\r\n");
    std::istringstream changed(std::string(65536, 'a') + "\n");
    require(content_digest(large) != content_digest(changed));

    const TemporaryContent content;
    const auto& root = content.root();
    const std::array files = {"data/common/systems.json",
                              "data/common/foliage/scatter_set.json",
                              "data/common/creatures/species/example.json",
                              "data/ui/main_menu.rml",
                              "res/shaders/water.frag",
                              "config/game-asset-packs.json",
                              "assets/audio/music/themes/main_menu_theme.mp3",
                              "game-assets/tree-small-02/1.0.0/data/models/trees/tree.lmesh"};
    for (const auto* file : files) {
        std::filesystem::create_directories((root / file).parent_path());
        std::ofstream output(root / file, std::ios::binary);
        output << "original\r\n";
        output.close();
        require(output.good());
    }
    const auto original = workload_content(root);
    require(original.size() == files.size());
    const auto identity = render_identity(original);
    for (const auto* file : files) {
        require(original.at(file) == content_digest(root / file));
        {
            std::ofstream output(root / file, std::ios::binary);
            output << "changed\r\n";
        }
        const auto updated = workload_content(root);
        require(updated.at(file) != original.at(file));
        require(render_identity(updated) != identity);
        {
            std::ofstream output(root / file, std::ios::binary);
            output << "original\r\n";
        }
        require(workload_content(root) == original);
    }
    require(std::filesystem::remove(root / files.back()));
    require(render_identity(workload_content(root)) != identity);
}

template<class Callback>
void require_refusal(Callback callback) {
    bool refused = false;
    try {
        callback();
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);
}

std::string native_path_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

void link_directory(const std::filesystem::path& link, const std::filesystem::path& target) {
    const auto native_link = std::filesystem::path(link).make_preferred();
    const auto native_target = std::filesystem::path(target).make_preferred();
#ifdef _WIN32
    // Real NTFS junctions do not require symbolic-link privilege.
    require(native_link.wstring().find_first_of(L"\"%\r\n") == std::wstring::npos);
    require(native_target.wstring().find_first_of(L"\"%\r\n") == std::wstring::npos);
    const auto command = L"cmd.exe /d /v:off /c mklink /j \"" + native_link.wstring() + L"\" \"" +
                         native_target.wstring() + L"\" >nul";
    const auto status = _wsystem(command.c_str());
    if (status != 0)
        throw std::runtime_error("junction creation failed: " +
                                 json{{"link", native_path_utf8(native_link)},
                                      {"target", native_path_utf8(native_target)},
                                      {"exit_status", status}}
                                     .dump());
#else
    std::filesystem::create_directory_symlink(native_target, native_link);
#endif
}

void saved_authority() {
    const TemporaryContent content;
    content.write("save/world.json", "{\"worldId\":\"capture-world\",\"seed\":424242}\r\n");
    content.write("save/chunks/part.bin", std::string("\0\1\r\n", 4));
    const auto directory = content.root() / "save";
    std::string recorded;
    const auto save = [&](bool) {
        recorded = saved_world_digest(directory);
        return ActionResult{};
    };
    const auto load = [&](bool) {
        require_workload_world_load(true, "capture-world", "capture-world");
        require_saved_world_digest(directory, recorded);
        return ActionResult{};
    };
    const std::vector<Traversal::Action> actions = {
        {.tick = 1, .name = "save", .position = {}, .radius = 0},
        {.tick = 2, .name = "reload", .position = {}, .radius = 0}};
    const auto edit = [](const Traversal::Action&, bool) {
        return ActionResult{};
    };
    const auto menu = [](bool) {
        return ActionResult{};
    };
    const auto observe = [](const Traversal::Action&, const ActionResult&) {
    };
    std::size_t cursor = 0;
    require(dispatch_actions(actions, cursor, 1, edit, save, load, menu, menu, observe));
    require_saved_world_digest(directory, recorded);
    content.write("copy/chunks/part.bin", std::string("\0\1\r\n", 4));
    content.write("copy/world.json", "{\"worldId\":\"capture-world\",\"seed\":424242}\r\n");
    require(saved_world_digest(content.root() / "copy") == recorded);
    content.write("save/chunks/part.bin", std::string("\0\1\n\r", 4));
    require_refusal([&] { require_saved_world_digest(directory, recorded); });
    content.write("save/chunks/part.bin", std::string("\0\1\r\n", 4));
    // A valid replacement keeps the world ID, file length and timestamp.
    const auto stamp = std::filesystem::last_write_time(directory / "world.json");
    content.write("save/world.json", "{\"worldId\":\"capture-world\",\"seed\":424243}\r\n");
    std::filesystem::last_write_time(directory / "world.json", stamp);
    require_refusal(
        [&] { dispatch_actions(actions, cursor, 2, edit, save, load, menu, menu, observe); });
    content.write("save/world.json", "{\"worldId\":\"capture-world\",\"seed\":424242}\r\n");
    require_saved_world_digest(directory, recorded);
    require(dispatch_actions(actions, cursor, 2, edit, save, load, menu, menu, observe));
    require(cursor == actions.size());
    content.write("save/chunks/extra.bin", "outside authority");
    require_refusal([&] { require_saved_world_digest(directory, recorded); });
    require(std::filesystem::remove(directory / "chunks/extra.bin"));
    std::filesystem::rename(directory / "chunks/part.bin", directory / "chunks/renamed.bin");
    require_refusal([&] { require_saved_world_digest(directory, recorded); });
    require(std::filesystem::remove(directory / "chunks/renamed.bin"));
    require_refusal([&] { require_saved_world_digest(directory, recorded); });
    content.write("save/chunks/part.bin", std::string("\0\1\r\n", 4));
    require_saved_world_digest(directory, recorded);
    require_refusal([&] { require_saved_world_digest(directory, {}); });
}

void snapshot_authority() {
    const TemporaryContent content;
    content.write("save/world_info.json", "original\r\n");
    content.write("save/region/payload", std::string("\0\1\r\n", 4));
    const auto directory = content.root() / "save";
    const auto recorded = saved_world_digest(directory);
    std::filesystem::path snapshot_root;
    {
        const VerifiedSaveSnapshot snapshot(directory, recorded, "capture");
        snapshot_root = snapshot.root();
        const auto copy = snapshot_root / "worlds/saves/capture";
        require(saved_world_digest(copy) == recorded);
        // Replace the live file after verification, before the consumer opens.
        content.write("save/world_info.json", "replaced\r\n");
        content.write("save/region/payload", "changed");
        require(saved_world_digest(copy) == recorded);
        require_refusal(
            [&] { const VerifiedSaveSnapshot rejected(directory, recorded, "capture"); });
        require(saved_world_digest(copy) == recorded);
    }
    require(!std::filesystem::exists(snapshot_root));
    const std::ifstream live(directory / "world_info.json", std::ios::binary);
    std::ostringstream live_bytes;
    live_bytes << live.rdbuf();
    require(live_bytes.str() == "replaced\r\n");
    for (const std::string id : {"", ".", "..", "a/b", "a\\b", "C:save"})
        require_refusal([&] { const VerifiedSaveSnapshot rejected(directory, recorded, id); });
    require_refusal([&] { const VerifiedSaveSnapshot rejected(directory, {}, "capture"); });
    std::filesystem::create_directory(content.root() / "empty");
    const auto empty_digest = saved_world_digest(content.root() / "empty");
    require_refusal([&] {
        const VerifiedSaveSnapshot rejected(content.root() / "empty", empty_digest, "capture");
    });
    require_refusal([&] { const SavedFileInput missing(directory / "missing"); });
    require_refusal([&] { const SavedFileInput special(directory); });
    // A sparse file exercises the byte ceiling before any proportional allocation.
    content.write("oversize/payload", "");
    std::filesystem::resize_file(content.root() / "oversize/payload", 512ull * 1024 * 1024 + 1);
    require_refusal([&] { saved_world_digest(content.root() / "oversize"); });
#ifndef _WIN32
    // No writer is needed: a blocking open here would hang on this FIFO.
    require(::mkfifo((directory / "pipe").c_str(), 0600) == 0);
    require_refusal([&] { const SavedFileInput special(directory / "pipe"); });
    require_refusal([&] { saved_world_digest(directory); });
#endif
}

void observation_records() {
    ObservationRecords<WorkloadObservation, kMaxFrames> frames(2);
    require(frames.size() == 0 && frames.remaining() == 2);
    TransitionRecords transitions(2);
    transitions.append({{1, -1}, 0, 1});
    transitions.append({{2, -2}, 1, 0});
    WorkloadObservation first;
    first.tick = 1;
    first.frame = 400;
    first.state = "gameplay";
    first.ui_document = "hud";
    first.far_requested = "legacy";
    first.far_effective = "legacy";
    first.transitions_end = 2;
    first.cave_requested = 0;
    first.settled = true;
    frames.append(first);
    const auto* address = &frames.at(0);
    frames.append({});
    require(&frames.at(0) == address && frames.remaining() == 0);
    const auto row = workload_observation_json(frames.at(0), transitions);
    require(row.at("tick") == 1 && row.at("frame") == 400);
    require(row.at("cave_requested") == 0 && row.at("cave_effective").is_null());
    require(row.at("transitions").size() == 2);
    require(row.at("transitions")[1].at("region") == json::array({2, -2}));
    require(row.at("settled") == true && row.at("state") == "gameplay");
    for (const char* flag : {"paused",
                             "underground",
                             "camera_in_air",
                             "stream_promotion_pending",
                             "region_scheduler_fault",
                             "foliage_pending",
                             "world_ready",
                             "settled"})
        require(row.at(flag).is_boolean());
    require_refusal([&] { frames.append({}); });
    require(frames.size() == 2 && frames.at(0).frame == 400);
    require_refusal([&] { transitions.append({}); });
    const auto invalid_capacity = [](auto create) {
        bool refused = false;
        try {
            create();
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        require(refused);
    };
    invalid_capacity(
        [] { (void)ObservationRecords<WorkloadObservation, kMaxFrames>(kMaxFrames + 1); });
    invalid_capacity([] { (void)TransitionRecords(kMaxCaptureTransitions + 1); });
    ObservationRecords<ActionObservation, 64> empty(0);
    require_refusal([&] { empty.append({}); });
    bool missing = false;
    try {
        (void)frames.at(2);
    } catch (const std::out_of_range&) {
        missing = true;
    }
    require(missing);
}

template<class Exception, class Callback>
void require_exception(Callback callback) {
    bool refused = false;
    try {
        callback();
    } catch (const Exception&) {
        refused = true;
    }
    require(refused);
}

template<class T, std::size_t Limit>
constexpr bool observation_record_values(const T& first, const T& second) {
    ObservationRecords<T, Limit> records(2);
    if (records.size() != 0 || records.remaining() != 2)
        return false;
    records.append(first);
    const auto* address = &records.at(0);
    auto copy = records;
    records.append(second);
    if (&records.at(0) != address || records.size() != 2 || records.remaining() != 0 ||
        copy.size() != 1 || copy.remaining() != 1 || &copy.at(0) == address)
        return false;
    copy.append(second);
    ObservationRecords<T, Limit> partial(2);
    partial.append(first);
    const auto& partial_source = partial;
    auto partial_moved = std::move(partial);
    if (partial_source.size() != 0 || partial_source.remaining() != 0 ||
        partial_moved.size() != 1 || partial_moved.remaining() != 1)
        return false;
    partial_moved.append(second);
    ObservationRecords<T, Limit> assigned(0);
    assigned = copy;
    if (assigned.size() != 2 || assigned.remaining() != 0)
        return false;
    // Observe through an existing const view, including after ownership moves.
    const auto& source = records;
    auto moved = std::move(records);
    if (source.size() != 0 || source.remaining() != 0 || &moved.at(0) != address)
        return false;
    const auto& moved_source = moved;
    assigned = std::move(moved);
    return moved_source.size() == 0 && moved_source.remaining() == 0 && assigned.size() == 2 &&
           assigned.remaining() == 0 && &assigned.at(0) == address;
}

// Exercise the production implementation during compilation as well as in the
// registered native case. The four record types share the same fixed buffer.
static_assert(observation_record_values<CameraObservation, kMaxFrames>({}, {}));
static_assert(observation_record_values<ActionObservation, 64>({}, {}));
static_assert(observation_record_values<WorkloadObservation, kMaxFrames>({}, {}));
static_assert(observation_record_values<TransitionObservation, kMaxCaptureTransitions>({}, {}));

struct AssignmentObservation {
    const ObservationRecords<AssignmentObservation, 1>* records = nullptr;
    bool* observed_empty = nullptr;
    constexpr AssignmentObservation& operator=(const AssignmentObservation& record) {
        *record.observed_empty = record.records->size() == 0;
        return *this;
    }
};

static_assert([] {
    ObservationRecords<AssignmentObservation, 1> records(1);
    bool observed_empty = false;
    records.append({.records = &records, .observed_empty = &observed_empty});
    return observed_empty && records.size() == 1;
}());

template<class T, std::size_t Limit, class Make, class Identify>
void observation_record_bounds(Make make, Identify identify) {
    using Records = ObservationRecords<T, Limit>;
    const auto empty = [&](Records& records) {
        require(records.size() == 0 && records.remaining() == 0);
        require_exception<std::out_of_range>([&] { (void)records.at(0); });
        require_refusal([&] { records.append(make(0)); });
    };
    const auto full = [&](Records& records, std::size_t capacity) {
        require(records.size() == capacity && records.remaining() == 0);
        require_refusal([&] { records.append(make(capacity)); });
        require_refusal([&] { records.append(make(capacity)); });
        require(records.size() == capacity && records.remaining() == 0);
        for (std::size_t index = 0; index < capacity; ++index)
            require(identify(records.at(index)) == identify(make(index)));
        require_exception<std::out_of_range>([&] { (void)records.at(capacity); });
        require_exception<std::out_of_range>(
            [&] { (void)records.at(std::numeric_limits<std::size_t>::max()); });
    };
    // Bound the test work by each actual production limit, including its last
    // valid slot. A capacity tied to vector allocation rounding must not grow.
    for (const std::size_t capacity :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, Limit - 1, Limit}) {
        Records records(capacity);
        require(records.size() == 0 && records.remaining() == capacity);
        require_exception<std::out_of_range>([&] { (void)records.at(0); });
        const T* address = nullptr;
        for (std::size_t index = 0; index < capacity; ++index) {
            records.append(make(index));
            if (index == 0)
                address = &records.at(0);
            require(&records.at(0) == address);
            require(records.size() == index + 1 && records.remaining() == capacity - index - 1);
            require_exception<std::out_of_range>([&] { (void)records.at(index + 1); });
        }
        full(records, capacity);
        auto copy = records;
        full(copy, capacity);
        Records assigned(capacity == 0 ? 2 : 0);
        assigned = records;
        full(assigned, capacity);
        // Self assignment must retain the count, capacity and every record.
        auto& alias = assigned;
        assigned = alias;
        full(assigned, capacity);
        auto moved = std::move(records);
        empty(records);
        if (capacity != 0)
            require(&moved.at(0) == address);
        full(moved, capacity);
        assigned = std::move(moved);
        empty(moved);
        full(assigned, capacity);
        assigned = std::move(alias);
        full(assigned, capacity);
        // Moved-from buffers can acquire and use fresh preallocated storage.
        records = Records(1);
        records.append(make(0));
        full(records, 1);
    }
    for (const std::size_t capacity : {Limit + 1, std::numeric_limits<std::size_t>::max()})
        require_exception<std::invalid_argument>([&] { (void)Records(capacity); });
}

struct FailingObservation {
    int value = 0;
    bool fail = false;
    FailingObservation& operator=(const FailingObservation& other) {
        if (other.fail)
            throw std::runtime_error("observation assignment failed");
        value = other.value;
        return *this;
    }
};

void observation_record_boundaries() {
    observation_record_bounds<CameraObservation, kMaxFrames>(
        [](std::size_t index) { return CameraObservation{.tick = static_cast<int>(index)}; },
        [](const CameraObservation& record) { return record.tick; });
    observation_record_bounds<ActionObservation, 64>(
        [](std::size_t index) {
            return ActionObservation{.tick = static_cast<int>(index), .name = "dig"};
        },
        [](const ActionObservation& record) { return record.tick; });
    observation_record_bounds<WorkloadObservation, kMaxFrames>(
        [](std::size_t index) {
            WorkloadObservation record;
            record.frame = index;
            return record;
        },
        [](const WorkloadObservation& record) { return record.frame; });
    observation_record_bounds<TransitionObservation, kMaxCaptureTransitions>(
        [](std::size_t index) {
            return TransitionObservation{
                .region = {static_cast<int>(index), 0}, .from = 0, .to = 1};
        },
        [](const TransitionObservation& record) { return record.region[0]; });
    ObservationRecords<CameraObservation, 0> no_capacity(0);
    require_refusal([&] { no_capacity.append({}); });
    require_exception<std::invalid_argument>(
        [] { (void)ObservationRecords<CameraObservation, 0>(1); });
    // This template limit fits size_t, but this allocation cannot fit the
    // allocator's difference/byte bound. Refuse before attempting allocation.
    using LargeRecords = ObservationRecords<unsigned char, std::numeric_limits<std::size_t>::max()>;
    LargeRecords small(1);
    small.append(7);
    require(small.at(0) == 7 && small.remaining() == 0);
    require_exception<std::invalid_argument>(
        [] { (void)LargeRecords(std::numeric_limits<std::size_t>::max()); });
    ObservationRecords<FailingObservation, 2> failing(2);
    failing.append({.value = 7});
    require_refusal([&] { failing.append({.value = 8, .fail = true}); });
    require(failing.size() == 1 && failing.remaining() == 1 && failing.at(0).value == 7);
    require_exception<std::out_of_range>([&] { (void)failing.at(1); });
    failing.append({.value = 9});
    require(failing.size() == 2 && failing.remaining() == 0 && failing.at(1).value == 9);
}

void workload_options() {
    for (const std::string option : {"--render-benchmark",
                                     "--render-benchmark-schema",
                                     "--render-benchmark-frames",
                                     "--render-benchmark-warmup",
                                     "--render-benchmark-screenshot",
                                     "--capture-size",
                                     "--load-world",
                                     "--perf-profile",
                                     "--traversal",
                                     "--runtime-root",
                                     "--save-root",
                                     "--declare-render-overrides",
                                     "--world-preset",
                                     "--cam-pos",
                                     "--cam-yaw",
                                     "--cam-pitch",
                                     "--window-mode"}) {
        for (const bool assigned : {false, true}) {
            for (const std::string value : {"1280x720", "0", ""}) {
                auto option_argument = option;
                if (assigned) {
                    option_argument += '=';
                    option_argument += value;
                }
                std::vector<std::string> arguments = {"client", option_argument};
                if (!assigned)
                    arguments.push_back(value);
                arguments.insert(arguments.end(), {"--render-scale=0", "successor"});
                std::vector<char*> argv;
                argv.reserve(arguments.size());
                for (auto& argument : arguments)
                    argv.push_back(argument.data());
                const auto overrides = render_overrides(static_cast<int>(argv.size()), argv.data());
                require(!overrides.contains("cli:" + option));
                require(overrides.at("cli:--render-scale") == "0");
                require(overrides.at("cli:successor") == "true");
            }
        }
    }
}

json path_diagnostics(const std::filesystem::path& path) {
    json result = {{"native", native_path_utf8(path)},
                   {"generic", Luminumbra::Filesystem::GenericUtf8PathString(path)},
                   {"lexically_normal", native_path_utf8(path.lexically_normal())}};
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    result["canonical"] = {{"path", native_path_utf8(canonical)},
                           {"error", error.value()},
                           {"message", error.message()}};
    const auto weakly_canonical = std::filesystem::weakly_canonical(path, error);
    result["weakly_canonical"] = {{"path", native_path_utf8(weakly_canonical)},
                                  {"error", error.value()},
                                  {"message", error.message()}};
    const auto status = std::filesystem::symlink_status(path, error);
    result["symlink_status"] = {{"type", static_cast<int>(status.type())},
                                {"error", error.value()},
                                {"message", error.message()}};
    return result;
}

void optional_audio() {
    const TemporaryContent content;
    Luminumbra::Filesystem::CaptureContentPaths policy;
    const auto check = [&](bool condition,
                           std::string_view expectation,
                           const json& actual,
                           const std::source_location& location = std::source_location::current()) {
        if (!condition)
            throw std::runtime_error(
                "optional audio assertion failed at line " + std::to_string(location.line()) +
                ": " +
                json{{"expectation", expectation},
                     {"actual", actual},
                     {"policy_active", policy.active()},
                     {"policy_refused", policy.refused()},
                     {"root", path_diagnostics(content.root())},
                     {"assets", path_diagnostics(content.root() / "assets")},
                     {"audio", path_diagnostics(content.root() / "assets/audio")},
                     {"included", path_diagnostics(content.root() / "assets/audio/included.ogg")},
                     {"escape", path_diagnostics(content.root() / "assets/audio/../private.ogg")}}
                    .dump(2));
    };
    const auto removed_audio = std::filesystem::remove(content.root() / "assets/audio");
    check(removed_audio, "remove initial audio directory returns true", removed_audio);
    policy.enable(content.root());
    check(policy.active(), "policy is active", policy.active());
    check(!policy.refused(), "policy has no refusal after enable", policy.refused());
    const auto absent = workload_content(content.root());
    check(absent.contains("assets/audio"), "absent inventory contains assets/audio", absent);
    check(absent.at("assets/audio").is_null(), "absent audio entry is null", absent);
    const auto identity = render_identity(absent);
    const auto created_audio = std::filesystem::create_directory(content.root() / "assets/audio");
    check(created_audio, "create empty audio directory returns true", created_audio);
    const auto empty = workload_content(content.root());
    check(!empty.contains("assets/audio"), "empty inventory omits absence entry", empty);
    const auto empty_identity = render_identity(empty);
    check(empty_identity != identity,
          "empty and absent identities differ",
          {{"empty", empty_identity}, {"absent", identity}, {"inventory", empty}});
    content.write("assets/audio/included.ogg", "audio bytes");
    const auto present = workload_content(content.root());
    check(!present.contains("assets/audio"), "present inventory omits absence entry", present);
    check(present.contains("assets/audio/included.ogg"),
          "present inventory contains audio file",
          present);
    const auto digest = content_digest(content.root() / "assets/audio/included.ogg");
    check(present.at("assets/audio/included.ogg") == digest,
          "audio digest matches file bytes",
          {{"inventory", present}, {"expected_digest", digest}});
    const auto present_identity = render_identity(present);
    check(present_identity != identity,
          "present and absent identities differ",
          {{"present", present_identity}, {"absent", identity}});
    check(present_identity != empty_identity,
          "present and empty identities differ",
          {{"present", present_identity}, {"empty", empty_identity}});
    const auto included_allowed = policy.allows(content.root() / "assets/audio/included.ogg");
    check(included_allowed, "policy allows included audio file", included_allowed);
    check(!policy.refused(), "policy has no refusal after included audio", policy.refused());
    const auto removed_file = std::filesystem::remove(content.root() / "assets/audio/included.ogg");
    check(removed_file, "remove audio file returns true", removed_file);
    const auto removed_empty = std::filesystem::remove(content.root() / "assets/audio");
    check(removed_empty, "remove empty audio directory returns true", removed_empty);
    const auto removed = workload_content(content.root());
    check(removed == absent,
          "removed inventory equals absent inventory",
          {{"removed", removed}, {"absent", absent}});
    content.write("assets/private.ogg", "outside audio tree");
    const auto escape_allowed = policy.allows(content.root() / "assets/audio/../private.ogg");
    check(!escape_allowed, "policy denies parent-relative private audio file", escape_allowed);
    check(policy.refused(), "policy records escaping audio refusal", policy.refused());
    // A missing tree cannot hide a redirected parent, including NTFS junctions.
    const auto removed_private = std::filesystem::remove(content.root() / "assets/private.ogg");
    check(removed_private, "remove private audio file returns true", removed_private);
    const auto removed_assets = std::filesystem::remove(content.root() / "assets");
    check(removed_assets, "remove assets directory returns true", removed_assets);
    const TemporaryContent outside;
    link_directory(content.root() / "assets", outside.root());
    Luminumbra::Filesystem::CaptureContentPaths redirected;
    bool enable_refused = false;
    try {
        redirected.enable(content.root());
    } catch (const std::runtime_error&) {
        enable_refused = true;
    }
    check(enable_refused,
          "enable rejects redirected parent with missing audio",
          {{"threw_runtime_error", enable_refused},
           {"redirected_active", redirected.active()},
           {"outside", path_diagnostics(outside.root())}});
    bool inventory_refused = false;
    json redirected_inventory;
    try {
        redirected_inventory = workload_content(content.root());
    } catch (const std::runtime_error&) {
        inventory_refused = true;
    }
    check(inventory_refused,
          "inventory rejects redirected parent with missing audio",
          {{"threw_runtime_error", inventory_refused},
           {"inventory", redirected_inventory},
           {"outside", path_diagnostics(outside.root())}});
    const auto removed_link = std::filesystem::remove(content.root() / "assets");
    check(removed_link, "remove redirected assets directory returns true", removed_link);
}

void audio_references() {
    const TemporaryContent content;
    content.write("assets/audio/music/themes/main_menu_theme.mp3", "audio bytes");
    Luminumbra::Filesystem::CaptureContentPaths policy;
    policy.enable(content.root());
    // These are the startup banks loaded with default audio enabled. Inspect
    // every file reference through the same containment policy as LoadBank.
    for (const char* bank : {"sfx_main.bank.json", "music.bank.json"}) {
        std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "data/audio" / bank);
        const auto document = json::parse(input);
        for (const auto& event : document.at("events"))
            for (const auto& file : event.at("files"))
                require(policy.allows(content.root() / file.get<std::string>()));
    }
    require(!policy.refused());
    const auto original = workload_content(content.root());
    const auto identity = render_identity(original);
    content.write("assets/audio/music/themes/main_menu_theme.mp3", "different audio bytes");
    require(workload_content(content.root()).at("assets/audio/music/themes/main_menu_theme.mp3") !=
            original.at("assets/audio/music/themes/main_menu_theme.mp3"));
    require(render_identity(workload_content(content.root())) != identity);
    content.write("assets/private.mp3", "outside audio tree");
    require(!policy.allows(content.root() / "assets/audio/../private.mp3"));
    require(policy.refused());
}

// Reproduce an entry escaping the scan root without privileged filesystem links.
class EscapingSpeciesDirectoryIterator {
public:
    EscapingSpeciesDirectoryIterator() = default;
    EscapingSpeciesDirectoryIterator(const std::filesystem::path& directory, std::error_code& error)
        : entry_(directory.parent_path().parent_path() / "external.json", error)
        , active_(!error) {}
    const std::filesystem::directory_entry& operator*() const {
        return entry_;
    }
    bool operator!=(const EscapingSpeciesDirectoryIterator& other) const {
        return active_ != other.active_;
    }
    void increment(std::error_code& error) {
        error.clear();
        active_ = false;
    }

private:
    std::filesystem::directory_entry entry_;
    bool active_ = false;
};

void species_capture_references(const TemporaryContent& content) {
    const auto directory = content.root() / "data/species";
    content.write("data/species/deer.json", R"({"id":"deer"})");
    content.write("external.json", R"({"id":"outside"})");
    luminumbra::ai::CreatureSpeciesRegistry registry;
    std::string parse_error;
    require(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
    std::size_t reads = 0;
    const Luminumbra::World::WorldgenFileReader reader = [&](const std::filesystem::path& file,
                                                             std::string& bytes) {
        ++reads;
        return Luminumbra::World::ReadWorldgenFile(file, bytes);
    };
    std::vector<std::string> errors;
    bool io_failed = false;
    require(registry.LoadFromDirectory(directory, errors, reader, &io_failed) == 1);
    require(!io_failed && errors.empty() && reads == 1);
    reads = 0;
    for (const bool report_failure : {false, true}) {
        errors.clear();
        io_failed = false;
        require(registry.LoadFromDirectory(
                    content.root(), errors, reader, report_failure ? &io_failed : nullptr) == 0);
        require(!errors.empty() && io_failed == report_failure && reads == 0);
        errors.clear();
        io_failed = false;
        require(registry.LoadFromDirectory<EscapingSpeciesDirectoryIterator>(
                    directory, errors, reader, report_failure ? &io_failed : nullptr) == 0);
        require(!errors.empty() && io_failed == report_failure && reads == 0);
        require(registry.size() == 2 && registry.FindByName("retained") != nullptr &&
                registry.FindByName("deer") != nullptr &&
                registry.FindByName("outside") == nullptr);
    }
    luminumbra::foliage::SpeciesRegistry foliage_registry;
    require(foliage_registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
    errors.clear();
    io_failed = false;
    require(foliage_registry.LoadFromDirectory(directory, errors, &io_failed) == 1);
    require(!io_failed && errors.empty());
    for (const bool report_failure : {false, true}) {
        errors.clear();
        io_failed = false;
        require(foliage_registry.LoadFromDirectory(
                    content.root(), errors, report_failure ? &io_failed : nullptr) == 0);
        require(!errors.empty() && io_failed == report_failure);
        errors.clear();
        io_failed = false;
        require(foliage_registry.LoadFromDirectory<EscapingSpeciesDirectoryIterator>(
                    directory, errors, report_failure ? &io_failed : nullptr) == 0);
        require(!errors.empty() && io_failed == report_failure);
        require(foliage_registry.size() == 2 && foliage_registry.Find("retained") != nullptr &&
                foliage_registry.Find("deer") != nullptr &&
                foliage_registry.Find("outside") == nullptr);
    }
}

void content_references() {
    const TemporaryContent content;
    content.write("data/texture.ltex", "inventoried bytes");
    content.write("external.ltex", "external bytes");
    Luminumbra::Filesystem::CaptureContentPaths policy;
    require(policy.allows(content.root() / "external.ltex"));
    policy.enable(content.root());
    for (const auto* tree : Luminumbra::Filesystem::kCaptureContentTrees) {
        content.write(std::filesystem::path(tree) / "included.json", "{}");
        require(policy.allows(content.root() / tree / "included.json"));
    }
    require(policy.allows(content.root() / "data/../res/included.json"));
    require(policy.allows(content.root() / "data/missing/asset.json"));
    // An existing file cannot be used as an ancestor of a missing asset.
    std::error_code resolution_error;
    require(Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / "data/texture.ltex/child",
                                                        resolution_error)
                .empty());
    require(static_cast<bool>(resolution_error));
    require(resolution_error == std::errc::not_a_directory);
    require_refusal([&] {
        Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / "data/texture.ltex/child");
    });
    for (const auto* suffix : {"child", "missing/child"}) {
        Luminumbra::Filesystem::CaptureContentPaths invalid_policy;
        invalid_policy.enable(content.root());
        require(!invalid_policy.allows(content.root() / "data/texture.ltex" / suffix));
        require(invalid_policy.refused());
    }
    const auto canonical_data =
        Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / "data");
    require(Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / "data/missing/asset.json",
                                                        resolution_error) ==
            canonical_data / "missing/asset.json");
    require(!resolution_error);
    require(Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / std::string(32768, 'x'),
                                                        resolution_error)
                .empty());
    require(resolution_error == std::errc::filename_too_long);
#ifdef _WIN32
    // The handle spelling must also accept 8.3 aliases, when the volume supplies them.
    // GetShortPathName returns the long spelling on volumes with short names disabled.
    std::array<wchar_t, 32768> short_root{}; // Extended Win32 path bound, with terminator.
    const DWORD short_length = GetShortPathNameW(
        content.root().c_str(), short_root.data(), static_cast<DWORD>(short_root.size()));
    require(short_length > 0 && short_length < short_root.size());
    const std::filesystem::path short_alias(short_root.data());
    Luminumbra::Filesystem::CaptureContentPaths short_policy;
    short_policy.enable(short_alias);
    require(short_policy.allows(short_alias / "data/texture.ltex"));
    require(short_policy.allows(content.root() / "data/texture.ltex"));
    const auto canonical_root = Luminumbra::Filesystem::WeaklyCanonicalPath(content.root());
    const auto mixed_alias = short_alias.parent_path() / content.root().filename();
    // Case changes exercise the mismatch even when the volume disables 8.3 names.
    auto case_name = content.root().filename().native();
    case_name.front() = L'R';
    const auto case_root = content.root().parent_path() / case_name;
    for (const auto& selected : {short_alias, mixed_alias, canonical_root, case_root}) {
        Luminumbra::Filesystem::CaptureContentPaths spelling_policy;
        spelling_policy.enable(selected);
        for (const auto& queried :
             {content.root(), short_alias, mixed_alias, canonical_root, case_root}) {
            require(spelling_policy.allows(queried / "data/texture.ltex"));
            require(spelling_policy.allows(queried / "DATA/TEXTURE.LTEX"));
            require(spelling_policy.allows(queried / "DATA/MISSING/asset.json"));
        }
        require(!spelling_policy.refused());
        require(!spelling_policy.allows(selected / "DATA/../external.ltex"));
        require(spelling_policy.refused());
    }
    // Stored directory case also participates in enable()'s redirect check.
    const TemporaryContent case_content;
    require(std::filesystem::remove(case_content.root() / "assets/audio"));
    require(std::filesystem::create_directory(case_content.root() / "assets/AUDIO"));
    case_content.write("assets/AUDIO/included.ogg", "audio bytes");
    Luminumbra::Filesystem::CaptureContentPaths case_policy;
    case_policy.enable(case_content.root());
    require(case_policy.allows(case_content.root() / "assets/audio/included.ogg"));
    require(case_policy.allows(case_content.root() / "assets/audio/missing.ogg"));
    require(!case_policy.refused());
    for (const auto& invalid_suffix : {std::filesystem::path("data/<invalid>"),
                                       std::filesystem::path("data/<invalid>~"),
                                       std::filesystem::path("data/texture.ltex/child"),
                                       std::filesystem::path("data/texture.ltex/missing~"),
                                       std::filesystem::path("data") / std::wstring(32768, L'x')}) {
        for (const auto& queried : {content.root(), short_alias}) {
            Luminumbra::Filesystem::CaptureContentPaths invalid_policy;
            invalid_policy.enable(queried);
            require(!invalid_policy.allows(queried / invalid_suffix));
            require(invalid_policy.refused());
        }
    }
    const auto native_root = std::filesystem::path(content.root()).make_preferred();
    if (!Luminumbra::Filesystem::IsWindowsUncPath(native_root)) {
        const std::filesystem::path extended_root(LR"(\\?\)" + native_root.native());
        Luminumbra::Filesystem::CaptureContentPaths extended_policy;
        extended_policy.enable(extended_root);
        require(extended_policy.allows(extended_root / "data/texture.ltex"));
        require(extended_policy.allows(content.root() / "data/texture.ltex"));
    }
    const auto invalid = Luminumbra::Filesystem::WeaklyCanonicalPath(
        content.root() / "data/<invalid>", resolution_error);
    require(invalid.empty() && static_cast<bool>(resolution_error));
    require(Luminumbra::Filesystem::WeaklyCanonicalPath(content.root() / std::wstring(32768, L'x'),
                                                        resolution_error)
                .empty());
    require(resolution_error == std::errc::filename_too_long);
#endif
    require(!policy.refused());
    // Both absolute and parent-relative references resolve outside the inventory.
    require(!policy.allows(content.root() / "external.ltex"));
    require(!policy.allows(content.root() / "data/../external.ltex"));
    require(!policy.allows(content.root() / "data/../../external.ltex"));
    require(policy.refused());
    require(policy.allows(content.root() / "data/texture.ltex"));
    require(policy.refused()); // A subsequent fallback cannot clear refusal.

    // Exercise the public JSON dependency loader with the shared capture policy.
    std::string bytes;
    require(Luminumbra::World::ReadWorldgenFile(content.root() / "external.ltex", bytes));
    Luminumbra::Filesystem::capture_content_paths.enable(content.root());
    require(Luminumbra::World::ReadWorldgenFile(content.root() / "res/included.json", bytes));
    require(bytes == "{}");
    require(!Luminumbra::World::ReadWorldgenFile(content.root() / "external.ltex", bytes));
    require(bytes.empty());
    require(!Luminumbra::World::ReadWorldgenFile(content.root() / "data/../external.ltex", bytes));
    require(bytes.empty());
    require(Luminumbra::Filesystem::capture_content_paths.refused());

    species_capture_references(content);

    // Both command arguments include generic separators and shell-sensitive characters.
    content.write("outside & (target)!/nested/included.json", "{}");
    const auto link = content.root() / "data/redirected & (link)!";
    const auto outside = content.root() / "outside & (target)!/nested";
    link_directory(link, content.root() / "res");
    require(policy.allows(link / "included.json"));
    require(policy.allows(link / "missing.json"));
    require(std::filesystem::remove(link));
    // A successful load must not cache validation across a later link replacement.
    link_directory(link, outside);
    require(!policy.allows(link / "included.json"));
    require(!policy.allows(link / "missing.json"));
    require(!Luminumbra::World::ReadWorldgenFile(link / "included.json", bytes));
    require(bytes.empty());
    require_refusal([&] { workload_content(content.root()); });
    require(std::filesystem::remove(link));

    // Both the selected root spelling and its canonical spelling remain usable.
    const TemporaryContent aliases;
    const auto alias = aliases.root() / "alias";
    link_directory(alias, content.root());
    Luminumbra::Filesystem::CaptureContentPaths aliased_policy;
    aliased_policy.enable(alias);
    require(aliased_policy.allows(alias / "data/texture.ltex"));
    require(aliased_policy.allows(content.root() / "data/texture.ltex"));
    require(!aliased_policy.refused());
    // Resolving names for lexical comparison must not resolve an unselected link.
    require(!policy.allows(alias / "data/texture.ltex"));
    require(!policy.allows(alias / "data/missing/asset.json"));
    require(workload_content(alias) == workload_content(content.root()));
    require(std::filesystem::remove(alias));
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "observation-records") {
            observation_records();
            observation_record_boundaries();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "saved-authority") {
            saved_authority();
            snapshot_authority();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "content-references") {
            content_references();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "audio-references") {
            audio_references();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "optional-audio") {
            optional_audio();
            return 0;
        }
        require(argc == 1);
        workload_options();
        observation_records();
        observation_record_boundaries();
        configuration();
        graphics_capture_configuration();
        simulation_configuration();
        content_configuration();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
