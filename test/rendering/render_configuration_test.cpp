#include "luminumbra_client/app/RenderConfiguration.h"
#include "luminumbra_client/app/RenderWorkload.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldgenFileReader.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace Luminumbra::Client::Measurement;
using luminumbra::core::SystemConfig;
using nlohmann::json;

void require(bool condition) {
    if (!condition)
        throw std::runtime_error("render configuration assertion failed");
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

void content_configuration() {
    std::istringstream empty;
    std::istringstream hello("hello");
    require(content_digest(empty) == "fnv1a64:cbf29ce484222325");
    require(content_digest(hello) == "fnv1a64:a430d84680aabd0b");
    std::istringstream large(std::string(65536, 'a') + "\r\n");
    std::istringstream changed(std::string(65536, 'a') + "\n");
    require(content_digest(large) != content_digest(changed));

    const auto root = std::filesystem::temp_directory_path() /
                      ("render-content-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    const std::array files = {"data/common/systems.json",
                              "data/common/foliage/scatter_set.json",
                              "data/common/creatures/species/example.json",
                              "data/ui/main_menu.rml",
                              "res/shaders/water.frag",
                              "config/game-asset-packs.json",
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

struct TemporaryContent {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("render-authority-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryContent() {
        require(std::filesystem::create_directory(root));
        for (const auto* tree : Luminumbra::Filesystem::kCaptureContentTrees)
            require(std::filesystem::create_directory(root / tree));
    }
    ~TemporaryContent() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void write(const std::filesystem::path& relative, const std::string& bytes) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        require(output.good());
    }
};

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

void link_directory(const std::filesystem::path& link, const std::filesystem::path& target) {
#ifdef _WIN32
    // Real NTFS junctions do not require symbolic-link privilege.
    require(link.wstring().find_first_of(L"\"%\r\n") == std::wstring::npos);
    require(target.wstring().find_first_of(L"\"%\r\n") == std::wstring::npos);
    const auto command =
        L"cmd.exe /d /c mklink /j \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
    require(_wsystem(command.c_str()) == 0);
#else
    std::filesystem::create_directory_symlink(target, link);
#endif
}

void saved_authority() {
    const TemporaryContent content;
    content.write("save/world.json", "{\"worldId\":\"capture-world\",\"seed\":424242}\r\n");
    content.write("save/chunks/part.bin", std::string("\0\1\r\n", 4));
    const auto directory = content.root / "save";
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
    require(saved_world_digest(content.root / "copy") == recorded);
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

void content_references() {
    const TemporaryContent content;
    content.write("data/texture.ltex", "inventoried bytes");
    content.write("external.ltex", "external bytes");
    Luminumbra::Filesystem::CaptureContentPaths policy;
    require(policy.allows(content.root / "external.ltex"));
    policy.enable(content.root);
    for (const auto* tree : Luminumbra::Filesystem::kCaptureContentTrees) {
        content.write(std::filesystem::path(tree) / "included.json", "{}");
        require(policy.allows(content.root / tree / "included.json"));
    }
    require(policy.allows(content.root / "data/../res/included.json"));
    require(!policy.refused());
    // Both absolute and parent-relative references resolve outside the inventory.
    require(!policy.allows(content.root / "external.ltex"));
    require(!policy.allows(content.root / "data/../external.ltex"));
    require(!policy.allows(content.root / "data/../../external.ltex"));
    require(policy.refused());
    require(policy.allows(content.root / "data/texture.ltex"));
    require(policy.refused()); // A subsequent fallback cannot clear refusal.

    // Exercise the public JSON dependency loader with the shared capture policy.
    std::string bytes;
    require(Luminumbra::World::ReadWorldgenFile(content.root / "external.ltex", bytes));
    Luminumbra::Filesystem::capture_content_paths.enable(content.root);
    require(Luminumbra::World::ReadWorldgenFile(content.root / "res/included.json", bytes));
    require(bytes == "{}");
    require(!Luminumbra::World::ReadWorldgenFile(content.root / "external.ltex", bytes));
    require(bytes.empty());
    require(!Luminumbra::World::ReadWorldgenFile(content.root / "data/../external.ltex", bytes));
    require(bytes.empty());
    require(Luminumbra::Filesystem::capture_content_paths.refused());

    content.write("outside/included.json", "{}");
    const auto link = content.root / "data/redirected";
    const auto outside = content.root / "outside";
    link_directory(link, outside);
    require(!policy.allows(link / "included.json"));
    require(!Luminumbra::World::ReadWorldgenFile(link / "included.json", bytes));
    require(bytes.empty());
    require_refusal([&] { workload_content(content.root); });
    require(std::filesystem::remove(link));

    // Both the selected root spelling and its canonical spelling remain usable.
    const TemporaryContent aliases;
    const auto alias = aliases.root / "alias";
    link_directory(alias, content.root);
    Luminumbra::Filesystem::CaptureContentPaths aliased_policy;
    aliased_policy.enable(alias);
    require(aliased_policy.allows(alias / "data/texture.ltex"));
    require(aliased_policy.allows(content.root / "data/texture.ltex"));
    require(!aliased_policy.refused());
    require(workload_content(alias) == workload_content(content.root));
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
        require(argc == 1);
        configuration();
        simulation_configuration();
        content_configuration();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
