#pragma once

//  SystemConfig — data-driven feature-flag + per-system tuning registry.
// One registry (data/common/systems.json)
// queried by every system so each can be turned on/off + tuned from data, no recompile.
//
// Determinism contract (verified against persistence/WorldPersistenceRoundtrip.h):
//   * Flags split into `sim.*` and `render.*` (presentation, except circadian tuning).
//   * ComputeConfigSubHash retains the diagnostic config:v1 serialization.
//     GameSession computes effective identity for joins and replays
//     independently of the established per-tick world_hash.
//   * render.* flags never appear in the diagnostic sub-hash. The session identity
//     also covers render.circadian because its amplitude is applied to simulation.
//   * enabled is an O(1) bit test on a resolved immutable snapshot (no map lookup,
//     no string compare, no allocation) so it is safe on the 36k-entity / 30 Hz tick.

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>

#include <glm/glm.hpp>

#include "DeveloperBindingSetting.h"
#include "DeveloperPanelLayout.h"
#include "GraphicsPresets.h"
#include "SystemConfigRegistry.gen.h" // X-macro registry GENERATED from ConfigSchema.json

namespace luminumbra::core {

// Settings overlays have their own version, independent of world/save/protocol formats.
inline constexpr std::uint32_t kSettingsSchemaVersion = 1;

enum class SettingsLoadState : std::uint8_t {
    FirstLaunch,
    Loaded,
    MigratedLegacy,
    Recovery
};
struct SettingsLoadReport {
    SettingsLoadState state = SettingsLoadState::FirstLaunch;
    bool needs_first_launch_detection = true; // Hook only; no hardware probing here.
    std::string recovery_reason;
};

// Developer records are bounded during parsing; unknown stable ids survive overlays.
// Runtime effects and session history are deliberately absent from this model.
struct DeveloperSettings {
    static constexpr std::size_t kRecordLimit = 256;
    static constexpr std::size_t kIdLimit = 128;
    int schema_version = 1;
    std::map<std::string, bool> widgets;
    std::map<std::string, DeveloperBindingSetting> bindings;
    std::map<std::string, DeveloperPanelLayout> layouts;
    bool WidgetVisible(const std::string& id, bool default_visible) const {
        const auto it = widgets.find(id);
        return it == widgets.end() ? default_visible : it->second;
    }
    bool PerfHudVisible() const {
        return WidgetVisible("perf.hud", true);
    }
};

// Player-facing settings (client-only; NEVER hashed). Persisted to a writable per-user
// overlay (%APPDATA%/Luminumbra/settings.json).
struct UserSettings {
    DeveloperSettings developer;
    // video
    std::string resolution;                 // "" = native/default; else "WxH"
    std::string window_mode = "borderless"; // windowed | borderless | fullscreen
    bool vsync = true;                      // Saved user choices override this default.
    int frame_cap = 0;                      // 0 = unlimited; choices in kFrameCaps.
    float fov = 45.0f;
    float render_scale = 1.0f; // Compatibility mirror on load; graphics owns persistence.
    float ui_scale = 1.0f; // HUD/UI density-independent-pixel ratio (0.5..2.5); 1.0 = native px.
                           // Lets players scale the HUD up on big/4K/ultrawide displays.
    float mouse_sensitivity = 0.025f; // 25% of the prior 0.1 default (owner request 2026-06-18)
    // audio (0..1)
    float audio_master = 1.0f;
    float audio_sfx = 1.0f;
    float audio_music = 1.0f;
    // controls: logical InputAction name -> GLFW key code. Ordered for deterministic save.
    std::map<std::string, int> keybinds;
    GraphicsSettings graphics;
};

// Compile-time registry of every system flag. sim.* entries first, then render.*.
// GENERATED from ConfigSchema.json: the members below expand from LUMIN_CONFIG_KEY_TABLE
// (SystemConfigRegistry.gen.h), so this enum, the kKeys registry (SystemConfig.cpp) and the
// config-hash residency share ONE authored home and cannot drift. To add/rename a flag, edit
// ConfigSchema.json then re-run `python tools/config_codegen.py --emit
// src/luminumbra_common/core/SystemConfigRegistry.gen.h`. Canonical order == schema order ==
// the hash's serialization order == the packed bit position; append-only, never reorder.
enum class SysKey : std::uint8_t {
#define LUMIN_CONFIG_EMIT_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY) ENUM,
    LUMIN_CONFIG_KEY_TABLE(LUMIN_CONFIG_EMIT_KEY)
#undef LUMIN_CONFIG_EMIT_KEY
    Count
};

// Compile-time registry of tunable params (globally-unique ids; each owned by a SysKey).
// GENERATED from ConfigSchema.json via LUMIN_CONFIG_PARAM_TABLE. Defaults are authored once
// in the schema (and cross-checked against the owning-system constants by --check); explicitly
// disabling sim tuning uses the compiled behaviour. Same edit path as SysKey.
enum class SysParam : std::uint8_t {
#define LUMIN_CONFIG_EMIT_PARAM(ENUM, OWNER, JSON_NAME, IS_VEC3, SCALAR, VX, VY, VZ) ENUM,
    LUMIN_CONFIG_PARAM_TABLE(LUMIN_CONFIG_EMIT_PARAM)
#undef LUMIN_CONFIG_EMIT_PARAM
    Count
};

// Values supplied to the canonical serializer. Session callers supply current effective
// values, including tuning changed through public setters. No owner-based fallback here.
struct SimulationIdentityValues {
    std::array<bool, static_cast<std::size_t>(SysKey::Count)> enabled{};
    std::array<double, static_cast<std::size_t>(SysParam::Count)> scalars{};
    std::array<glm::vec3, static_cast<std::size_t>(SysParam::Count)> vectors{};
    // Session options outside the SystemConfig registry, in canonical wire order.
    bool finite_hydrology_enabled = true;
    std::int32_t weather_rain_scale_mm = 25;
    std::uint64_t region_work_limit = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t region_hold_ticks = 30;
    std::uint8_t region_reduced_shift = 1;

    SimulationIdentityValues();
    [[nodiscard]] std::string ComputeIdentity() const;
};

class SystemConfig {
public:
    SystemConfig();
    // Every sim.* registry flag, including tuning gates that select compiled defaults.
    [[nodiscard]] std::map<std::string, bool> SimulationGates() const;
    // All shipping flags on, including the far-world ladder; explicit enabled:false opts out.
    // Params unset (caller fallback). sim.* hashed; render.* excluded from the config sub-hash.
    [[nodiscard]] static SystemConfig Defaults();
    // Parse a systems.json document. Malformed/empty/blank text -> all-defaults (no throw).
    [[nodiscard]] static SystemConfig FromJsonString(const std::string& json_text);
    // Load from disk. Missing/unreadable file -> all-defaults (no throw).
    [[nodiscard]] static SystemConfig LoadFromFile(const std::filesystem::path& path);
    // Runtime defaults come only from this root, including missing/unreadable-file fallback.
    // An optional overlay changes user settings only, as in LoadLayered.
    [[nodiscard]] static SystemConfig
    LoadFromRuntimeRoot(const std::filesystem::path& root,
                        const std::filesystem::path& overlay_path = {});
    // Explicit CLI overrides must be readable JSON objects; failure leaves config unchanged.
    // Success replaces only system flags/tuning, retaining player settings and their load report.
    [[nodiscard]] static bool
    TryLoadFromFile(const std::filesystem::path& path, SystemConfig& config, std::string& error);

    // Replace all sim/render flags and tuning, including resetting omitted params to fallback.
    // Keep this config's player settings and load report together, so systems overrides/reloads
    // cannot clear Recovery protection or first-launch detection state. Ignores source user.*.
    void ReplaceSystemsFrom(const SystemConfig& source);

    // O(1) hot-path query: single bit test on the resolved flag set.
    [[nodiscard]] bool enabled(SysKey key) const {
        return (m_enabled >> static_cast<unsigned>(key)) & 1u;
    }

    // Returns the explicitly-configured value if the JSON named it, else `fallback`.
    [[nodiscard]] float param(SysParam id, float fallback) const;
    [[nodiscard]] glm::vec3 param3(SysParam id, glm::vec3 fallback) const;

    // Additive sim-only sub-hash. Empty "" when all sim flags are disabled; deterministic and
    // order-independent (canonical SysKey/SysParam enum order) otherwise. render.*/user.* excluded.
    [[nodiscard]] std::string ComputeConfigSubHash() const;

    // ---- user.* player settings (client-only, never hashed) ----
    [[nodiscard]] const UserSettings& user() const {
        return m_user;
    }
    [[nodiscard]] UserSettings& user() {
        return m_user;
    }
    // Resolved keybind: the overlay's binding for `action`, else `fallback` (e.g. a client
    // compiled default). Keeps core engine-generic (no InputAction enum dependency here).
    [[nodiscard]] int keybind(const std::string& action, int fallback) const;

    // Parse ONLY the `user.*` section from `json_text` and overlay it onto this config
    // (sim.*/render.* ignored). Invalid input keeps values unchanged and reports
    // Recovery, blocking saves to protect the original overlay. Numeric overflow outside
    // developer layouts also throws, retaining the strict parser refusal.
    void OverlayUserFromJsonString(const std::string& json_text);

    [[nodiscard]] const SettingsLoadReport& settings_load_report() const {
        return m_settings_load_report;
    }

    // Load layering: FromJsonString(defaults file) then OverlayUserFromJsonString(overlay file).
    [[nodiscard]] static SystemConfig LoadLayered(const std::filesystem::path& defaults_path,
                                                  const std::filesystem::path& overlay_path);

    // Write ONLY the user.* section to `path` (atomic; creates parent dirs). Returns false on
    // I/O/validation failure, or while in Recovery (protects the original file).
    // Deterministic key order (keybinds and graphics overrides are ordered maps).
    bool SaveUserOverlay(const std::filesystem::path& path) const;

    // Per-user overlay location: %APPDATA%/Luminumbra/settings.json (Windows) or
    // $XDG_CONFIG_HOME/luminumbra/settings.json (POSIX), with HOME fallbacks.
    [[nodiscard]] static std::filesystem::path DefaultUserOverlayPath();

private:
    static constexpr std::size_t kParamCount = static_cast<std::size_t>(SysParam::Count);

    static_assert(static_cast<unsigned>(SysKey::Count) < 32);
    std::uint32_t m_enabled = (1u << static_cast<unsigned>(SysKey::Count)) - 1u;
    std::uint64_t m_param_set = 0;                 // which params were explicitly set (<=64 params)
    std::array<glm::vec3, kParamCount> m_params{}; // scalar params live in .x
    UserSettings m_user{};                         // client-only; never hashed
    SettingsLoadReport m_settings_load_report{};
};

} // namespace luminumbra::core
