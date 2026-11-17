#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace Luminumbra::Client {

// Presentation diagnostics only: never serialized into saves or simulation identity.
struct AudioVolumes {
    float master = 1.0f;
    float music = 1.0f;
    float sfx = 1.0f;
    bool operator==(const AudioVolumes&) const = default;
};

struct AudioBackendState {
    const char* mode = "uninitialized";
    bool playback_enabled = false;
    bool engine_initialized = false;
    bool device_started = false;
    bool sfx_routed = false;
    bool ambient_routed = false;
    bool events_routed = false;
    bool ui_routed = false;
    std::optional<AudioVolumes> playback_volumes;
    bool telemetry_enabled = false;
    std::size_t retained_requests = 0;
};

struct AudioControlState {
    // The settings form has exactly three playback controls. Zero also represents
    // an unopened/unavailable form; partial forms must never count as observed.
    std::size_t present = 0;
    std::size_t disabled = 0;
};

struct AudioAcceptanceRequest {
    bool no_audio = false;
    bool enable_audio = false;
    bool telemetry = false;
};

inline nlohmann::json AudioVolumesToJson(const std::optional<AudioVolumes>& volumes) {
    if (!volumes) {
        return nullptr;
    }
    return {{"master", volumes->master}, {"music", volumes->music}, {"sfx", volumes->sfx}};
}

inline nlohmann::json BuildAudioAcceptance(const AudioAcceptanceRequest& request,
                                           const std::optional<AudioVolumes>& loaded,
                                           const std::optional<AudioVolumes>& current,
                                           const AudioBackendState& backend,
                                           const AudioControlState& controls) {
    const bool observed = controls.present == 3;
    const nlohmann::json disabled =
        observed ? nlohmann::json(controls.disabled == 3) : nlohmann::json(nullptr);
    const nlohmann::json precedence = request.no_audio
                                          ? nlohmann::json(std::string(backend.mode) == "null")
                                          : nlohmann::json(nullptr);
    return {{"schema", "luminumbra.audio_acceptance.v1"},
            {"resolution",
             {{"no_audio_requested", request.no_audio},
              {"enable_audio_requested", request.enable_audio},
              {"requested_mode", request.no_audio ? "null" : "device"},
              {"resolved_mode", backend.mode},
              // There is no persisted playback-enable setting; saved levels are independent.
              {"reason", request.no_audio ? "flag" : "default"}}},
            {"backend",
             {{"playback_enabled", backend.playback_enabled},
              {"engine_initialized", backend.engine_initialized},
              {"device_started", backend.device_started},
              {"routing",
               {{"sfx", backend.sfx_routed},
                {"ambient", backend.ambient_routed},
                {"events", backend.events_routed},
                {"ui", backend.ui_routed}}}}},
            {"saved_volumes",
             {{"loaded", AudioVolumesToJson(loaded)},
              {"in_effect", AudioVolumesToJson(current)},
              {"unchanged",
               loaded && current ? nlohmann::json(*loaded == *current) : nlohmann::json(nullptr)},
              // Null/unavailable playback has no mixer gains, even with nonzero preferences.
              {"playback_gains", AudioVolumesToJson(backend.playback_volumes)}}},
            {"controls",
             {{"observed", observed},
              {"present", controls.present},
              {"disabled_count", controls.disabled},
              {"match_playback",
               observed ? nlohmann::json(controls.disabled == (backend.playback_enabled ? 0u : 3u))
                        : nlohmann::json(nullptr)}}},
            {"telemetry",
             {{"opt_in_requested", request.telemetry},
              {"enabled", backend.telemetry_enabled},
              {"retained_requests", backend.retained_requests},
              {"opt_in_only", !backend.telemetry_enabled || request.telemetry}}},
            // Aliases for the historical checklist names. Unobserved is deliberately null.
            {"no_audio_precedence", precedence},
            {"controls_disabled", disabled}};
}

} // namespace Luminumbra::Client
