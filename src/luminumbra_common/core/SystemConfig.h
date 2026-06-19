#pragma once

// §1a SystemConfig — data-driven feature-flag + per-system tuning registry.
// Spec: .forge/specs/system-config/spec.md. One registry (data/common/systems.json)
// queried by every system so each can be turned on/off + tuned from data, no recompile.
//
// Determinism contract (verified against persistence/WorldPersistenceRoundtrip.h):
//   * Flags split into `sim.*` (may affect sim state) and `render.*` (render-only).
//   * ComputeConfigSubHash() is an ADDITIVE per-system sub-hash (modelled on wind/
//     weather/aether): it does NOT alter the top-level world_hash, and it is empty
//     ("") whenever every sim.* flag is at its compiled default -> the canonical
//     baseline d950a6afc12a5cdc and the sub-hash SET both stay byte-identical.
//   * render.* flags NEVER appear in the sub-hash and never touch any hash.
//   * enabled() is an O(1) bit test on a resolved immutable snapshot (no map lookup,
//     no string compare, no allocation) so it is safe on the 36k-entity / 30 Hz tick.

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include <glm/glm.hpp>

namespace luminumbra::core {

// Player-facing settings (client-only; NEVER hashed). Persisted to a writable per-user
// overlay (%APPDATA%/Luminumbra/settings.json). Spec Addendum A; docs/STANDARDS.md §5/§6.
struct UserSettings {
    // video
    std::string resolution;                 // "" = native/default; else "WxH"
    std::string window_mode = "borderless"; // windowed | borderless | fullscreen
    bool vsync = false;                      // default OFF: preserve today's uncapped 300fps target
    float fov = 45.0f;
    float render_scale = 1.0f;
    float mouse_sensitivity = 0.025f;  // 25% of the prior 0.1 default (owner request 2026-06-18)
    // audio (0..1)
    float audio_master = 1.0f;
    float audio_sfx = 1.0f;
    float audio_music = 1.0f;
    // controls: logical InputAction name -> GLFW key code. Ordered for deterministic save.
    std::map<std::string, int> keybinds;
};

// Compile-time registry of every system flag. sim.* entries first, then render.*.
// New systems append a key here (single compile-checked source of truth for keys).
enum class SysKey : std::uint8_t {
    // --- sim.* (may change sim state; included in ComputeConfigSubHash when enabled) ---
    SimPlantGrowth = 0,
    SimErosion,
    // --- render.* (render-only; never hashed) ---
    RenderMoonlight,
    RenderTreeWind,
    RenderPlantProcgen,  // grow scattered plants via the procgen instead of baked models
    Count
};

// Compile-time registry of tunable params (globally-unique ids; each owned by a SysKey).
enum class SysParam : std::uint8_t {
    PlantMutationRate = 0,  // sim.plant_growth.mutation_rate (scalar)
    MoonlightStrength,      // render.moonlight.strength (scalar)
    MoonlightColor,         // render.moonlight.color (vec3)
    Count
};

class SystemConfig {
public:
    // All flags off, all params unset (queries return the caller's fallback).
    [[nodiscard]] static SystemConfig Defaults();
    // Parse a systems.json document. Malformed/empty/blank text -> all-defaults (no throw).
    [[nodiscard]] static SystemConfig FromJsonString(const std::string& json_text);
    // Load from disk. Missing/unreadable file -> all-defaults (no throw).
    [[nodiscard]] static SystemConfig LoadFromFile(const std::string& path);

    // O(1) hot-path query: single bit test on the resolved flag set.
    [[nodiscard]] bool enabled(SysKey key) const {
        return (m_enabled >> static_cast<unsigned>(key)) & 1u;
    }

    // Returns the explicitly-configured value if the JSON named it, else `fallback`.
    [[nodiscard]] float param(SysParam id, float fallback) const;
    [[nodiscard]] glm::vec3 param3(SysParam id, glm::vec3 fallback) const;

    // Additive sim-only sub-hash. Empty "" at all sim defaults; deterministic and
    // order-independent (canonical SysKey/SysParam enum order) otherwise. render.*/user.* excluded.
    [[nodiscard]] std::string ComputeConfigSubHash() const;

    // ---- user.* player settings (client-only, never hashed) ----
    [[nodiscard]] const UserSettings& user() const { return m_user; }
    [[nodiscard]] UserSettings& user() { return m_user; }
    // Resolved keybind: the overlay's binding for `action`, else `fallback` (e.g. a client
    // compiled default). Keeps core engine-generic (no InputAction enum dependency here).
    [[nodiscard]] int keybind(const std::string& action, int fallback) const;

    // Parse ONLY the `user.*` section from `json_text` and overlay it onto this config
    // (sim.*/render.* ignored). Malformed -> no change. This is the per-user overlay load.
    void OverlayUserFromJsonString(const std::string& json_text);

    // Load layering: FromJsonString(defaults file) then OverlayUserFromJsonString(overlay file).
    [[nodiscard]] static SystemConfig LoadLayered(const std::string& defaults_path,
                                                  const std::string& overlay_path);

    // Write ONLY the user.* section to `path` (atomic; creates parent dirs). Returns false on
    // I/O failure. Deterministic key order (keybinds are an ordered map).
    bool SaveUserOverlay(const std::string& path) const;

    // Per-user overlay location: %APPDATA%/Luminumbra/settings.json (Windows) or
    // $XDG_CONFIG_HOME/luminumbra/settings.json (POSIX), with HOME fallbacks.
    [[nodiscard]] static std::string DefaultUserOverlayPath();

private:
    static constexpr std::size_t kParamCount = static_cast<std::size_t>(SysParam::Count);

    std::uint32_t m_enabled = 0;                       // packed flag bitset (<=32 keys)
    std::uint32_t m_param_set = 0;                     // which params were explicitly set
    std::array<glm::vec3, kParamCount> m_params{};     // scalar params live in .x
    UserSettings m_user{};                             // client-only; never hashed
};

}  // namespace luminumbra::core
