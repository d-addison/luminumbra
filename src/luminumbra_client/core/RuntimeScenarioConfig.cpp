#include "core/RuntimeScenarioConfig.h"
#include "core/Log.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/SurfaceWorkBudget.h"
#include "rendering/DiagnosticCapture.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

bool HasCommandLineOption(int argc, char* argv[], const std::string& flag) {
    const std::string assignment_prefix = flag + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == flag || argument.starts_with(assignment_prefix))
            return true;
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

namespace {
std::string ResolveRuntimeScenario(int argc, char* argv[]) {
    auto scenario = GetCommandLineOption(argc, argv, "--scenario", "");
    // Preserve the launch-wrapper fallback: a nonempty CLI scenario wins.
    if (scenario.empty()) {
        const auto sweep_env = Core::ReadEnvironment("LUMINUMBRA_VISUAL_SWEEP");
        if (sweep_env && !sweep_env->empty() && sweep_env->front() != '0') {
            scenario = "world_visual_sweep";
        }
    }
    return scenario;
}

// Parses "WxH" (e.g. "1600x900") into width/height. Returns false (leaving the
// outputs untouched) on any malformed/non-positive value.
bool ParseResolution(const std::string& value, int& out_width, int& out_height) {
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

void ParseSurfaceRadiusOptions(int argc,
                               char* argv[],
                               int& surface_radius,
                               int& collision_radius,
                               int& benchmark_stream_radius) {
    // Validate both spellings and every occurrence before any scenario can
    // create a world. Missing/invalid values must not fall back to a default.
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        for (const auto flag : {std::string_view("--horizon-radius"),
                                std::string_view("--collision-radius"),
                                std::string_view("--render-benchmark-stream-radius")}) {
            std::string_view value;
            if (argument == flag) {
                if (i + 1 < argc)
                    value = argv[++i];
            } else if (argument.starts_with(flag) && argument.size() > flag.size() &&
                       argument[flag.size()] == '=') {
                value = argument.substr(flag.size() + 1);
            } else {
                continue;
            }
            int& radius = flag == "--horizon-radius"     ? surface_radius
                          : flag == "--collision-radius" ? collision_radius
                                                         : benchmark_stream_radius;
            if (!World::TryParseSurfaceRadius(value, radius))
                throw std::invalid_argument(std::string(flag) + " requires 0..12 chunks");
        }
    }
    if (HasCommandLineOption(argc, argv, "--render-benchmark-stream-radius") &&
        GetCommandLineOption(argc, argv, "--render-benchmark-schema", "v2") != "v4")
        throw std::invalid_argument("--render-benchmark-stream-radius requires schema v4");
}
} // namespace

RuntimePathOptions ParseRuntimePathOptions(int argc, char* argv[]) {
    (void)ParseCaptureFeatureProfile(argc, argv);
    const auto temporal_audit = ParseTemporalAuditRuntimeOptions(argc, argv);
    // Admission precedes --save-root directory creation.
    const auto diagnostic_environment = Core::ReadEnvironment("LUMIN_CAPTURE_DIAGNOSTICS");
    const auto diagnostics = Rendering::ResolveDiagnosticCaptureOptions(
        std::span<char* const>(argv + 1, static_cast<std::size_t>(std::max(0, argc - 1))),
        diagnostic_environment ? std::optional<std::string_view>(*diagnostic_environment)
                               : std::nullopt);
    // Temporal admission above owns its fixed site, ROI and producer exclusions.
    if (diagnostics.enabled) {
        if (!temporal_audit) {
            for (const auto* parity : {"--render-parity-frame", "--upscale-seam-parity"}) {
                if (!GetCommandLineOption(argc, argv, parity, "").empty()) {
                    throw std::invalid_argument(std::string("capture diagnostics refuse ") +
                                                parity +
                                                "; parity redispatches require separate captures");
                }
            }
            for (const auto* producer : {"--render-parity-ssao",
                                         "--bake-tree-impostor",
                                         "--survey",
                                         "--ui-screenshot",
                                         "--ui-thumbs",
                                         "--scene-config",
                                         "--timelapse",
                                         "--timelapse-frames"}) {
                if (HasCommandLineOption(argc, argv, producer)) {
                    throw std::invalid_argument(std::string("capture diagnostics refuse ") +
                                                producer + "; completed-frame adapter unavailable");
                }
            }
            const auto scenario = ResolveRuntimeScenario(argc, argv);
            if (!scenario.empty() && (HasCommandLineOption(argc, argv, "--frame-scan") ||
                                      HasCommandLineOption(argc, argv, "--render-benchmark"))) {
                throw std::invalid_argument("capture diagnostics refuse multiple capture drivers");
            }
            // These producers transform pixels, resize/redispatch targets or require forward
            // contributors that the completed-frame adapter cannot yet attribute.
            // Admission must precede save-root and scenario artifact creation.
            for (const auto* unsupported : {"window_mode_stress_smoke",
                                            "material_visual_smoke",
                                            "farlod_horizon_smoke",
                                            "water_visual_smoke",
                                            "weather_visual_smoke",
                                            "particle_emitter_determinism_smoke",
                                            "foliage_visual_smoke",
                                            "precipitation_smoke"}) {
                if (scenario == unsupported) {
                    throw std::invalid_argument(std::string("capture diagnostics refuse ") +
                                                unsupported +
                                                "; producer or extent coverage unavailable");
                }
            }
            const bool scenario_capture =
                scenario == "world_visual_sweep" || scenario == "lod_seam_arrival_smoke" ||
                scenario == "lod_ground_smoke" || scenario == "skybox_visual_smoke" ||
                scenario == "cloud_shadow_smoke" || scenario == "timeofday_sweep_smoke" ||
                scenario == "player_view_smoke" || scenario == "skinned_mesh_visual_smoke" ||
                scenario == "creature_slice_smoke";
            const bool benchmark =
                !GetCommandLineOption(argc, argv, "--render-benchmark", "").empty() &&
                !GetCommandLineOption(argc, argv, "--render-benchmark-screenshot", "").empty();
            if ((!scenario.empty() && !scenario_capture) ||
                (scenario.empty() && GetCommandLineOption(argc, argv, "--frame-scan", "").empty() &&
                 !benchmark)) {
                throw std::invalid_argument(
                    "capture diagnostics refuse unsupported capture mode: " + scenario);
            }
        }
    }
    int surface_radius = 0;
    int collision_radius = 0;
    int benchmark_stream_radius = 0;
    ParseSurfaceRadiusOptions(
        argc, argv, surface_radius, collision_radius, benchmark_stream_radius);
    RuntimePathOptions options;
    options.diagnostics = diagnostics;
    for (const char* flag : {"--runtime-root", "--save-root"}) {
        std::filesystem::path& path =
            std::string(flag) == "--runtime-root" ? options.runtime_root : options.save_root;
        bool seen = false;
        const std::string prefix = std::string(flag) + "=";
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument != flag && argument.rfind(prefix, 0) != 0)
                continue;
            if (seen)
                throw std::invalid_argument(std::string(flag) + " may only be supplied once");
            seen = true;
            std::string value;
            if (argument == flag) {
                if (i + 1 < argc)
                    value = argv[++i];
            } else {
                value = argument.substr(prefix.size());
            }
            path = std::filesystem::path(value);
            if (path.empty() || !Filesystem::IsAbsolutePath(path))
                throw std::invalid_argument(std::string(flag) + " requires an absolute directory");
        }
    }
    if (!options.save_root.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.save_root, error);
        if (error || !std::filesystem::is_directory(options.save_root, error) || error)
            throw std::invalid_argument("--save-root cannot be created: " +
                                        options.save_root.string());
    }
    return options;
}

RuntimeScenarioConfig
ParseRuntimeScenarioConfig(int argc, char* argv[], const std::filesystem::path& root_dir) {
    RuntimeScenarioConfig config;
    config.temporal_audit = ParseTemporalAuditRuntimeOptions(argc, argv);
    config.capture_feature_profile = ParseCaptureFeatureProfile(argc, argv);
    ParseSurfaceRadiusOptions(
        argc, argv, config.horizon_radius, config.collision_radius, config.benchmark_stream_radius);
    config.scenario = ResolveRuntimeScenario(argc, argv);
    config.systems_config_path = GetCommandLineOption(argc, argv, "--systems-config", "");
    config.preset_json_path = GetCommandLineOption(argc, argv, "--preset-json", "");
    config.auto_create_world = HasCommandLineFlag(argc, argv, "--auto-create-world");
    config.auto_enter_world = HasCommandLineFlag(argc, argv, "--auto-enter-world");
    config.no_audio = HasCommandLineFlag(argc, argv, "--no-audio");
    config.enable_audio = true; // --enable-audio remains accepted for compatibility.
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
        Luminumbra::Filesystem::ContentInput archetype_in(root_dir / config.creature_archetype);
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
        config.window_mode_stress_smoke() || config.world_visual_sweep() || config.temporal_audit) {
        config.auto_create_world = true;
        config.auto_enter_world = true;
    }

    const std::filesystem::path default_artifact_dir =
        root_dir / "build/debug/test-artifacts/runtime";
    const std::filesystem::path default_crash_dir = root_dir / "build/debug/crashes";
    config.artifact_dir =
        GetCommandLineOption(argc, argv, "--runtime-artifact-dir", default_artifact_dir.string());
    // Disabled normal play must not write a telemetry JSON on every listener update.
    config.audio_telemetry_path = GetCommandLineOption(argc, argv, "--audio-telemetry-path", "");
    config.crash_dir = GetCommandLineOption(argc, argv, "--crash-dir", default_crash_dir.string());
    config.hang_watchdog_seconds = std::max(
        0, std::atoi(GetCommandLineOption(argc, argv, "--hang-watchdog-seconds", "0").c_str()));
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

// Capture feature selection: keep admission independent of world/output setup.
RuntimeScenarioConfig::FeatureProfile ParseCaptureFeatureProfile(int argc, char* argv[]) {
    constexpr std::string_view flag = "--capture-feature-profile";
    auto profile = RuntimeScenarioConfig::FeatureProfile::None;
    bool seen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::string_view value;
        if (argument == flag) {
            if (index + 1 < argc) {
                value = argv[++index];
            }
        } else if (argument.starts_with("--capture-feature-profile=")) {
            value = argument.substr(flag.size() + 1);
        } else {
            continue;
        }
        if (seen || (value != "none" && value != "all_features")) {
            throw std::invalid_argument(
                "--capture-feature-profile requires one selection: none or all_features");
        }
        seen = true;
        profile = value == "all_features" ? RuntimeScenarioConfig::FeatureProfile::AllFeatures
                                          : RuntimeScenarioConfig::FeatureProfile::None;
    }
    return profile;
}

} // namespace Luminumbra::Client::ScenarioHarness

// Temporal plan admission: shared by both startup paths and planning callers.
namespace Luminumbra::Client::ScenarioHarness {
std::optional<Rendering::TemporalAuditPlan> ParseTemporalAuditPlanOptions(int argc, char* argv[]) {
    constexpr std::array<std::string_view, 13> flags = {"--temporal-audit-plan",
                                                        "--temporal-start-tick",
                                                        "--temporal-tick-rate",
                                                        "--temporal-day-ticks",
                                                        "--temporal-dawn-tick",
                                                        "--temporal-dusk-tick",
                                                        "--temporal-strike-tick",
                                                        "--temporal-frame-limit",
                                                        "--temporal-tick-limit",
                                                        "--temporal-moon-phase-milli",
                                                        "--temporal-site",
                                                        "--temporal-roi",
                                                        "--temporal-output-bytes"};
    std::array<bool, flags.size()> seen{};
    std::array<std::uint64_t, flags.size()> values = {
        0, 0, 0, 0, 0, 0, 0, Rendering::kTemporalMaxFrames, Rendering::kTemporalMaxTicks, 1000};
    Rendering::TemporalAuditSpan span = Rendering::TemporalAuditSpan::DayNight;
    bool any = false;
    // O(argc * 13), with fixed storage. Check every occurrence and both forms.
    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        const std::string_view argument(argv[argument_index]);
        if (!argument.starts_with("--temporal-"))
            continue;
        any = true;
        const auto equals = argument.find('=');
        const auto name = argument.substr(0, equals);
        const auto found = std::ranges::find(flags, name);
        if (found == flags.end())
            throw std::invalid_argument("unknown temporal plan option");
        const auto index = static_cast<std::size_t>(found - flags.begin());
        if (seen[index])
            throw std::invalid_argument(std::string(name) + " may only be supplied once");
        seen[index] = true;
        std::string_view value;
        if (equals != std::string_view::npos)
            value = argument.substr(equals + 1);
        else if (argument_index + 1 < argc)
            value = argv[++argument_index];
        if (value.empty())
            throw std::invalid_argument(std::string(name) + " requires a nonempty value");
        if (index == 10 || index == 11) {
            if (value != (index == 10 ? "spawn" : "full-frame")) {
                throw std::invalid_argument(
                    "temporal site/ROI unsupported: require spawn/full-frame");
            }
        } else if (index == 0) {
            if (value == "day-night")
                span = Rendering::TemporalAuditSpan::DayNight;
            else if (value == "weather-cycle")
                span = Rendering::TemporalAuditSpan::WeatherCycle;
            else if (value == "multi-day")
                span = Rendering::TemporalAuditSpan::MultiDay;
            else
                throw std::invalid_argument("unknown temporal span");
        } else {
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), values[index]);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                throw std::invalid_argument(std::string(name) + " requires an unsigned integer");
        }
    }
    if (!any)
        return std::nullopt;
    for (std::size_t index = 0; index <= 5; ++index) {
        if (!seen[index])
            throw std::invalid_argument(std::string(flags[index]) +
                                        " is required for plan admission");
    }
    if (values[2] > 1000 || values[7] > Rendering::kTemporalMaxFrames || values[9] > 1000)
        throw std::invalid_argument("temporal rate, frame limit or moon phase exceeds range");
    const Rendering::TemporalAuditRequest request{
        .span = span,
        .start_tick = values[1],
        .ticks_per_day = values[3],
        .ticks_per_second = static_cast<std::uint32_t>(values[2]),
        .dawn_tick = values[4],
        .dusk_tick = values[5],
        .strike_tick = values[6],
        .night_moon_phase_milli = static_cast<std::uint32_t>(values[9]),
        .tick_limit = values[8],
        .frame_limit = static_cast<std::size_t>(values[7])};
    auto admission = Rendering::BuildTemporalAuditPlan(request);
    if (!admission.plan)
        throw std::invalid_argument("temporal plan refused: " + admission.reason);
    return std::move(admission.plan);
}

std::optional<RuntimeTemporalAudit> ParseTemporalAuditRuntimeOptions(int argc, char* argv[]) {
    auto plan = ParseTemporalAuditPlanOptions(argc, argv);
    const auto scenario = GetCommandLineOption(argc, argv, "--scenario", "");
    if (!plan && scenario != "temporal_audit") {
        return std::nullopt;
    }
    if (!plan || scenario != "temporal_audit") {
        throw std::invalid_argument(
            "temporal execution requires --scenario=temporal_audit and a plan");
    }
    const auto& request = plan->request;
    if (request.start_tick != 0 || request.ticks_per_second != 30 ||
        request.ticks_per_day != 162000 || request.dawn_tick != 27000 ||
        request.dusk_tick != 135000) {
        throw std::invalid_argument(
            "temporal execution requires the fresh canonical 30 Hz calendar");
    }
    // These options select independent writers, cameras or session inputs. A
    // temporal run owns one fresh seed-424242 default world and one fixed view.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument.starts_with("--timelapse-") || argument.starts_with("--render-benchmark-")) {
            throw std::invalid_argument("temporal execution refuses other capture controls");
        }
    }
    for (const auto* incompatible : {"--load-world",
                                     "--systems-config",
                                     "--preset-json",
                                     "--world-preset",
                                     "--cam-pos",
                                     "--cam-look",
                                     "--cam-dir",
                                     "--debug-goto",
                                     "--play-paths",
                                     "--profile-fly",
                                     "--scene-config",
                                     "--timelapse",
                                     "--timelapse-frames",
                                     "--frame-scan",
                                     "--render-benchmark",
                                     "--survey",
                                     "--ui-screenshot",
                                     "--ui-thumbs",
                                     "--timed-run",
                                     "--isolation-layers",
                                     "--isolation-backdrop",
                                     "--traversal",
                                     "--capture-size",
                                     "--cam-yaw",
                                     "--cam-pitch",
                                     "--render-parity-frame",
                                     "--upscale-seam-parity",
                                     "--render-parity-ssao",
                                     "--bake-tree-impostor",
                                     "--render-benchmark-schema",
                                     "--debug-glass-pane",
                                     "--debug-view"}) {
        if (HasCommandLineOption(argc, argv, incompatible)) {
            throw std::invalid_argument(std::string("temporal execution refuses ") + incompatible);
        }
    }
    if (ParseCaptureFeatureProfile(argc, argv) ==
        RuntimeScenarioConfig::FeatureProfile::AllFeatures) {
        throw std::invalid_argument(
            "temporal execution refuses the unsupported all_features profile");
    }
    const auto environment = Core::ReadEnvironment("LUMIN_CAPTURE_DIAGNOSTICS");
    const auto diagnostics = Rendering::ResolveDiagnosticCaptureOptions(
        std::span<char* const>(argv + 1, static_cast<std::size_t>(std::max(0, argc - 1))),
        environment ? std::optional<std::string_view>(*environment) : std::nullopt);
    // The only admitted extent is compile-time fixed. Mirror A4's reservation:
    // 128 MiB registry, 64 MiB workspace + 4 KiB codec scratch, 64 KiB row,
    // two RGBA32UI planes and retained RGB. No GL header enters startup admission.
    constexpr std::uint64_t pixels = std::uint64_t{kCapturePinnedWidth} * kCapturePinnedHeight;
    static_assert(kCapturePinnedWidth > 0 && kCapturePinnedWidth <= 16384 &&
                  kCapturePinnedHeight > 0 && kCapturePinnedHeight <= 16384 && pixels <= 16777216);
    static_assert(pixels * (2 * sizeof(Rendering::DiagnosticId) + 3) + 192ULL * 1024 * 1024 + 4096 +
                      65536 <=
                  512ULL * 1024 * 1024);
    if (!diagnostics.enabled ||
        (diagnostics.probe && ((*diagnostics.probe)[0] >= kCapturePinnedWidth ||
                               (*diagnostics.probe)[1] >= kCapturePinnedHeight))) {
        throw std::invalid_argument("temporal execution requires admitted A4 diagnostics and ROI");
    }
    // A4 output: RGB (3), two RGB float planes (24), depth/material (8),
    // RGBA32UI (16) bytes/pixel. Reserve its conservative 64 MiB metadata
    // workspace too; this covers headers, draw JSON, probe and capture manifest.
    Rendering::TemporalSequenceBudget budget{.frame_bytes = pixels * 51 + 64ULL * 1024 * 1024,
                                             .manifest_bytes = 16ULL * 1024 * 1024,
                                             .output_bytes = Rendering::kTemporalMaxOutputBytes};
    if (HasCommandLineOption(argc, argv, "--temporal-output-bytes")) {
        const auto value = GetCommandLineOption(argc, argv, "--temporal-output-bytes", "");
        const auto result =
            std::from_chars(value.data(), value.data() + value.size(), budget.output_bytes);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
            throw std::invalid_argument("temporal output budget requires an unsigned integer");
        }
    }
    Rendering::TemporalAuditSequence admission(*plan, budget);
    if (!admission.refusal().empty()) {
        throw std::invalid_argument("temporal execution refused: " +
                                    std::string(admission.refusal()));
    }
    return RuntimeTemporalAudit{
        .plan = std::move(*plan), .budget = budget, .diagnostics = diagnostics};
}

Systems::WeatherSchedule BuildTemporalAuditWeatherSchedule(const RuntimeTemporalAudit& audit) {
    const auto& request = audit.plan.request;
    if (request.span != Rendering::TemporalAuditSpan::WeatherCycle || request.start_tick != 0 ||
        request.ticks_per_second != 30 || request.strike_tick < 1500 ||
        request.strike_tick >= 2700) {
        throw std::invalid_argument("temporal weather requires an admitted canonical weather plan");
    }
    using Category = Systems::WeatherCategory;
    return {.phases =
                {{.start_tick = 0, .category = Category::Clear, .precipitation = 0, .storm = 0},
                 {.start_tick = 600, .category = Category::Clear, .precipitation = 0, .storm = 0},
                 {.start_tick = 1500,
                  .category = Category::Rain,
                  .precipitation = 65535,
                  .storm = 65535},
                 {.start_tick = 2700,
                  .category = Category::Rain,
                  .precipitation = 65535,
                  .storm = 65535},
                 {.start_tick = 3600, .category = Category::Clear, .precipitation = 0, .storm = 0}},
            .strike_tick = request.strike_tick};
}
} // namespace Luminumbra::Client::ScenarioHarness
