#include "core/RuntimeScenarioConfig.h"
#include "core/Log.h"
#include "luminumbra_common/core/Environment.h"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Luminumbra::Client::ScenarioHarness {

std::atomic<uint64_t> g_gl_debug_message_count{0};
std::atomic<uint64_t> g_gl_debug_error_count{0};
std::atomic<uint64_t> g_gl_debug_warning_count{0};
std::atomic<uint64_t> g_gl_debug_notification_count{0};

GLDebugRuntimeStats CurrentGLDebugRuntimeStats() {
    return {g_gl_debug_message_count.load(std::memory_order_relaxed),
            g_gl_debug_error_count.load(std::memory_order_relaxed),
            g_gl_debug_warning_count.load(std::memory_order_relaxed),
            g_gl_debug_notification_count.load(std::memory_order_relaxed)};
}

bool HasCommandLineFlag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) {
            return true;
        }
    }
    return false;
}

std::string
GetCommandLineOption(int argc, char* argv[], const std::string& flag, const std::string& fallback) {
    const std::string assignment_prefix = flag + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == flag && i + 1 < argc) {
            return argv[i + 1];
        }
        if (arg.rfind(assignment_prefix, 0) == 0) {
            return arg.substr(assignment_prefix.size());
        }
    }
    return fallback;
}

int GetCommandLineIntOption(int argc, char* argv[], const std::string& flag, int fallback) {
    const std::string value = GetCommandLineOption(argc, argv, flag, {});
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::max(1, std::stoi(value));
    } catch (...) {
        LUMINUMBRA_CORE_WARN("Invalid integer value '{}' for {}; using {}", value, flag, fallback);
        return fallback;
    }
}

uint64_t
GetCommandLineUInt64Option(int argc, char* argv[], const std::string& flag, uint64_t fallback) {
    const std::string value = GetCommandLineOption(argc, argv, flag, {});
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stoull(value);
    } catch (...) {
        LUMINUMBRA_CORE_WARN(
            "Invalid unsigned integer value '{}' for {}; using {}", value, flag, fallback);
        return fallback;
    }
}

WindowMode ParseWindowMode(const std::string& value, WindowMode fallback) {
    if (value == "windowed")
        return WindowMode::Windowed;
    if (value == "borderless")
        return WindowMode::Borderless;
    if (value == "fullscreen")
        return WindowMode::Fullscreen;
    if (value == "headless")
        return WindowMode::Headless;
    return fallback;
}

const char* WindowModeName(WindowMode mode) {
    switch (mode) {
        case WindowMode::Windowed:
            return "windowed";
        case WindowMode::Borderless:
            return "borderless";
        case WindowMode::Fullscreen:
            return "fullscreen";
        case WindowMode::Headless:
            return "headless";
    }
    return "unknown";
}

nlohmann::json
CapturePinMetadata(WindowMode active_window_mode, int capture_width, int capture_height) {
    const bool pinned =
        capture_width == kCapturePinnedWidth && capture_height == kCapturePinnedHeight;
    return nlohmann::json{{"window_mode", WindowModeName(active_window_mode)},
                          {"capture_width", capture_width},
                          {"capture_height", capture_height},
                          {"pinned_width", kCapturePinnedWidth},
                          {"pinned_height", kCapturePinnedHeight},
                          {"pinned", pinned}};
}

// Parses "WxH" (e.g. "1600x900") into width/height. Returns false (leaving the
// outputs untouched) on any malformed/non-positive value.
static bool ParseResolution(const std::string& value, int& out_width, int& out_height) {
    const auto x_pos = value.find_first_of("xX");
    if (x_pos == std::string::npos || x_pos == 0 || x_pos + 1 >= value.size()) {
        return false;
    }
    try {
        const int w = std::stoi(value.substr(0, x_pos));
        const int h = std::stoi(value.substr(x_pos + 1));
        if (w <= 0 || h <= 0)
            return false;
        out_width = w;
        out_height = h;
        return true;
    } catch (...) {
        return false;
    }
}

RuntimeScenarioConfig
ParseRuntimeScenarioConfig(int argc, char* argv[], const std::filesystem::path& root_dir) {
    RuntimeScenarioConfig config;
    config.scenario = GetCommandLineOption(argc, argv, "--scenario", "");
    //  the visual-sweep capture matrix can be selected with the
    // LUMINUMBRA_VISUAL_SWEEP=1 env flag (in addition to --scenario), so existing
    // auto/server launch wrappers can opt in without a CLI change. An explicit
    // --scenario always wins.
    if (config.scenario.empty()) {
        const auto sweep_env = Core::ReadEnvironment("LUMINUMBRA_VISUAL_SWEEP");
        if (sweep_env && !sweep_env->empty() && sweep_env->front() != '0') {
            config.scenario = "world_visual_sweep";
        }
    }
    config.auto_create_world = HasCommandLineFlag(argc, argv, "--auto-create-world");
    config.auto_enter_world = HasCommandLineFlag(argc, argv, "--auto-enter-world");
    config.no_audio = HasCommandLineFlag(argc, argv, "--no-audio");
    config.no_ui = HasCommandLineFlag(argc, argv, "--no-ui");
    config.hidden_window = HasCommandLineFlag(argc, argv, "--hidden-window");

    // --- Window modes ---
    // borderless is the interactive default. --window-mode wins; the legacy
    // --hidden-window flag is equivalent to --window-mode headless. headless
    // implies a hidden window (gates/server use it), so the two stay in sync.
    config.window_mode =
        ParseWindowMode(GetCommandLineOption(argc, argv, "--window-mode", ""),
                        config.hidden_window ? WindowMode::Headless : WindowMode::Borderless);
    if (config.window_mode == WindowMode::Headless) {
        config.hidden_window = true;
    }
    ParseResolution(GetCommandLineOption(argc, argv, "--resolution", ""),
                    config.windowed_width,
                    config.windowed_height);

    config.isolation_layers = GetCommandLineOption(argc, argv, "--isolation-layers", "");
    config.isolation_backdrop = GetCommandLineOption(argc, argv, "--isolation-backdrop", "");
    {
        // #1b-lush: render-only foliage density multiplier (showcase/photo scenes).
        const std::string s = GetCommandLineOption(argc, argv, "--foliage-density-scale", "1.0");
        float v = 1.0f;
        try {
            v = std::stof(s);
        } catch (...) {
            v = 1.0f;
        }
        config.foliage_density_scale = (v > 0.0f && v <= 8.0f) ? v : 1.0f;
    }
    //  avatar showcase: avatar showcase row count (skinned_mesh_visual_smoke). Clamp to a
    // sane max so a typo can't spawn thousands of rigs.
    config.avatars = std::clamp(GetCommandLineIntOption(argc, argv, "--avatars", 0), 0, 32);
    config.replicated = HasCommandLineFlag(argc, argv, "--replicated");
    config.wildlife = HasCommandLineFlag(argc, argv, "--wildlife");
    if (config.wildlife && config.avatars < 2)
        config.avatars = 2; // animal + human
    config.readiness_timeout_seconds = GetCommandLineIntOption(
        argc, argv, "--readiness-timeout", config.readiness_timeout_seconds);
    config.horizon_radius =
        GetCommandLineIntOption(argc, argv, "--horizon-radius", config.horizon_radius);
    config.collision_radius =
        GetCommandLineIntOption(argc, argv, "--collision-radius", config.collision_radius);
    config.coverage_radius =
        GetCommandLineIntOption(argc, argv, "--coverage-radius", config.coverage_radius);
    config.min_renderable_chunks = static_cast<size_t>(GetCommandLineUInt64Option(
        argc, argv, "--min-renderable-chunks", config.min_renderable_chunks));
    config.min_collision_chunks = static_cast<size_t>(GetCommandLineUInt64Option(
        argc, argv, "--min-collision-chunks", config.min_collision_chunks));
    config.memory_watermark_mb = GetCommandLineUInt64Option(argc, argv, "--memory-watermark-mb", 0);
    config.persistence_phase = GetCommandLineOption(argc, argv, "--persistence-phase", "");
    config.persistence_session_dir =
        GetCommandLineOption(argc, argv, "--persistence-session-dir", "");
    config.world_preset = GetCommandLineOption(argc, argv, "--world-preset", "");
    config.creature_archetype = GetCommandLineOption(argc, argv, "--creature-archetype", "");

    // resolve the skinned-mesh UV texture set data-drivenly.
    // Explicit flags win; otherwise the creature slice reads its texture paths
    // from the game archetype JSON, and the noun-free skinned-mesh visual falls
    // back to a generic test texture. RenderPipeline never names this content.
    config.skinned_albedo_texture =
        GetCommandLineOption(argc, argv, "--skinned-albedo-texture", "");
    config.skinned_normal_texture =
        GetCommandLineOption(argc, argv, "--skinned-normal-texture", "");
    if (config.skinned_albedo_texture.empty() && !config.creature_archetype.empty()) {
        std::ifstream archetype_in(root_dir / config.creature_archetype);
        if (archetype_in.is_open()) {
            try {
                nlohmann::json archetype;
                archetype_in >> archetype;
                if (archetype.contains("creature")) {
                    const auto& creature = archetype.at("creature");
                    config.skinned_albedo_texture = creature.value("albedo_texture", std::string{});
                    config.skinned_normal_texture = creature.value("normal_texture", std::string{});
                }
            } catch (...) {
                // Leave empty: RenderPipeline keeps the flat fallback.
            }
        }
    }
    if (config.skinned_albedo_texture.empty() && config.skinned_mesh_visual_smoke()) {
        config.skinned_albedo_texture = "data/textures/test/skinned_test_albedo_256.ltex";
        config.skinned_normal_texture = "data/textures/test/skinned_test_normal_256.ltex";
    }

    const int default_timed_run =
        config.auto_world_smoke()
            ? 300
            : ((config.lod_ground_smoke() || config.water_visual_smoke() ||
                config.material_visual_smoke() || config.skybox_visual_smoke() ||
                config.weather_visual_smoke() || config.particle_emitter_determinism_smoke() ||
                config.timeofday_sweep_smoke() || config.lod_boundary_oscillation_smoke() ||
                config.lod_seam_arrival_smoke() || config.player_view_smoke() ||
                config.farlod_horizon_smoke() || config.skinned_mesh_visual_smoke() ||
                config.creature_slice_smoke() || config.window_mode_stress_smoke() ||
                config.world_visual_sweep())
                   ? 60
                   : 0);
    config.timed_run_seconds =
        GetCommandLineIntOption(argc, argv, "--timed-run", default_timed_run);

    if (config.auto_world_smoke() || config.lod_ground_smoke() || config.water_visual_smoke() ||
        config.material_visual_smoke() || config.skybox_visual_smoke() ||
        config.weather_visual_smoke() || config.particle_emitter_determinism_smoke() ||
        config.timeofday_sweep_smoke() || config.lod_boundary_oscillation_smoke() ||
        config.lod_seam_arrival_smoke() || config.persistence_roundtrip_smoke() ||
        config.player_view_smoke() || config.farlod_horizon_smoke() ||
        config.skinned_mesh_visual_smoke() || config.creature_slice_smoke() ||
        config.window_mode_stress_smoke() || config.world_visual_sweep()) {
        config.auto_create_world = true;
        config.auto_enter_world = true;
    }

    const std::filesystem::path default_artifact_dir =
        root_dir / "build/debug/test-artifacts/runtime";
    const std::filesystem::path default_audio_telemetry_path =
        root_dir / "build/debug/test-artifacts/audio/audio-telemetry.json";
    const std::filesystem::path default_crash_dir = root_dir / "build/debug/crashes";
    config.artifact_dir =
        GetCommandLineOption(argc, argv, "--runtime-artifact-dir", default_artifact_dir.string());
    config.audio_telemetry_path = GetCommandLineOption(
        argc, argv, "--audio-telemetry-path", default_audio_telemetry_path.string());
    config.crash_dir = GetCommandLineOption(argc, argv, "--crash-dir", default_crash_dir.string());
    return config;
}

std::tm UtcTime(std::time_t value) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    return tm;
}

std::string TimestampUtc() {
    const std::time_t now = std::time(nullptr);
    const std::tm tm = UtcTime(now);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string TimestampForFile() {
    const std::time_t now = std::time(nullptr);
    const std::tm tm = UtcTime(now);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return output.str();
}

nlohmann::json Vec3ToJson(const Luminumbra::Vec3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

nlohmann::json IVec3ToJson(const Luminumbra::IVec3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

} // namespace Luminumbra::Client::ScenarioHarness
