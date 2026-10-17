#include "luminumbra_client/app/RenderConfiguration.h"
#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_client/app/RenderWorkload.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldgenFileReader.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

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
        require(captured.at("base_preset") == luminumbra::core::GraphicsPresetName(preset));
        require(render_identity(captured) != original);
    }
    settings.overrides = {{"grass_density", 0.5}, {"taau", false}, {"ui_blur", std::string("off")}};
    const luminumbra::core::GraphicsOverrides session = {{"render_scale", 0.75}};
    require(luminumbra::core::ResolveGraphics(settings, session, resolved, error));
    const auto captured = graphics_configuration(settings, resolved);
    require(captured.at("saved_overrides").at("grass_density") == 0.5);
    require(captured.at("saved_overrides").at("taau") == false);
    require(captured.at("saved_overrides").at("ui_blur") == "off");
    require(captured.at("session_overrides").at("render_scale") == 0.75);
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
    link_directory(link, outside);
    require(!policy.allows(link / "included.json"));
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
    require(workload_content(alias) == workload_content(content.root()));
    require(std::filesystem::remove(alias));
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "saved-authority") {
            saved_authority();
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
