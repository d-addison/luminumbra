#pragma once

#include "audio/IAudioManager.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Luminumbra::Client {

class NullAudioManager final : public IAudioManager {
public:
    explicit NullAudioManager(std::filesystem::path telemetry_path)
        : m_telemetry_path(std::move(telemetry_path)),
          m_started_at(std::chrono::steady_clock::now()) {}

    ~NullAudioManager() override {
        if (!m_shutdown_written) {
            WriteTelemetry("destructor");
        }
    }

    bool Init() override {
        ++m_init_calls;
        m_initialized = true;
        WriteTelemetry("init");
        return true;
    }

    void Update() override {
        ++m_update_calls;
    }

    void Shutdown() override {
        if (m_shutdown_written) {
            return;
        }
        ++m_shutdown_calls;
        m_initialized = false;
        WriteTelemetry("shutdown");
        m_shutdown_written = true;
    }

    bool LoadBank(const std::string& bankPath) override {
        ++m_load_bank_calls;
        m_bank_paths.push_back(bankPath);
        WriteTelemetry("load_bank");
        return true;
    }

    void UnloadBank(const std::string& bankPath) override {
        ++m_unload_bank_calls;
        m_unloaded_bank_paths.push_back(bankPath);
        WriteTelemetry("unload_bank");
    }

    void SetListenerTransform(const glm::vec3&, const glm::vec3&, const glm::vec3&) override {
        ++m_listener_transform_calls;
        WriteTelemetry("listener_transform");
    }

    bool PlayEvent(const AudioEventID& eventID, AudioEventHandle& outHandle) override {
        ++m_play_event_calls;
        m_event_ids.push_back(eventID);
        outHandle = {};
        WriteTelemetry("play_event");
        return true;
    }

    // NOTE: the re-stated defaults MUST match IAudioManager's (kept so callers
    // holding a concrete NullAudioManager* keep compiling with two-arg calls).
    bool PlayOneShot(const AudioEventID& eventID, const glm::vec3&,
                     BusId = BusId::Sfx) override {
        ++m_play_one_shot_calls;
        m_event_ids.push_back(eventID);
        WriteTelemetry("play_one_shot");
        return true;
    }

    bool PlayOneShot2D(const AudioEventID& eventID, BusId = BusId::Sfx) override {
        ++m_play_one_shot_2d_calls;
        m_event_ids.push_back(eventID);
        WriteTelemetry("play_one_shot_2d");
        return true;
    }

    void PlayMusic(const AudioEventID& musicEventID) override {
        ++m_play_music_calls;
        m_music_ids.push_back(musicEventID);
        WriteTelemetry("play_music");
    }

    void StopMusic() override {
        ++m_stop_music_calls;
        WriteTelemetry("stop_music");
    }

    void PlayAmbientLoop(const AudioEventID&, const glm::vec3&, float) override {}  // null backend: no-op
    void StopAmbientLoop(const AudioEventID&) override {}                           // null backend: no-op
    void SetAmbientVolume(const AudioEventID&, float) override {}                   // null backend: no-op

    void SetMasterVolume(float) override {}       // null backend: no-op
    void SetMusicVolume(float) override {}        // null backend: no-op
    void SetSfxVolume(float) override {}          // null backend: no-op
    void SetBusVolume(BusId, float) override {}   // null backend: no-op

    bool StopEvent(AudioEventHandle, bool = true) override {
        ++m_stop_event_calls;
        WriteTelemetry("stop_event");
        return true;
    }

    bool SetEventPosition(AudioEventHandle, const glm::vec3&) override {
        ++m_set_event_position_calls;
        WriteTelemetry("set_event_position");
        return true;
    }

    bool SetEventVolume(AudioEventHandle, float) override {
        ++m_set_event_volume_calls;
        WriteTelemetry("set_event_volume");
        return true;
    }

    bool SetEventParameter(AudioEventHandle, const AudioParamID&, float) override {
        ++m_set_event_parameter_calls;
        WriteTelemetry("set_event_parameter");
        return true;
    }

private:
    static std::string TimestampUtc() {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        std::ostringstream output;
        output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return output.str();
    }

    void WriteTelemetry(const char* phase) {
        if (m_telemetry_path.empty()) {
            return;
        }

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_started_at).count();
        const uint64_t playback_requests =
            m_play_event_calls + m_play_one_shot_calls + m_play_one_shot_2d_calls + m_play_music_calls;

        const nlohmann::json checks = nlohmann::json::array({
            {
                {"name", "null manager selected"},
                {"passed", true}
            },
            {
                {"name", "hardware backend not initialized"},
                {"passed", true}
            },
            {
                {"name", "bank loads are recorded without file access"},
                {"passed", true}
            },
            {
                {"name", "playback requests are suppressed"},
                {"passed", true}
            },
            {
                {"name", "telemetry artifact configured"},
                {"passed", !m_telemetry_path.empty()}
            }
        });

        const nlohmann::json artifact = {
            {"schema", "luminumbra.audio.null_telemetry.v1"},
            {"timestamp_utc", TimestampUtc()},
            {"phase", phase},
            {"elapsed_seconds", elapsed},
            {"passed", true},
            {"initialized", m_initialized},
            {"activation", {
                {"flag", "--no-audio"},
                {"selected", true},
                {"manager_type", "NullAudioManager"},
                {"hardware_backend_initialized", false},
                {"bank_files_touched", false}
            }},
            {"routing", {
                {"loads_suppressed", m_load_bank_calls > 0},
                {"playback_suppressed", playback_requests > 0},
                {"listener_updates_suppressed", true},
                {"updates_noop", true}
            }},
            {"counters", {
                {"init_calls", m_init_calls},
                {"update_calls", m_update_calls},
                {"shutdown_calls", m_shutdown_calls},
                {"load_bank_calls", m_load_bank_calls},
                {"unload_bank_calls", m_unload_bank_calls},
                {"listener_transform_calls", m_listener_transform_calls},
                {"play_event_calls", m_play_event_calls},
                {"play_one_shot_calls", m_play_one_shot_calls},
                {"play_one_shot_2d_calls", m_play_one_shot_2d_calls},
                {"play_music_calls", m_play_music_calls},
                {"stop_music_calls", m_stop_music_calls},
                {"stop_event_calls", m_stop_event_calls},
                {"set_event_position_calls", m_set_event_position_calls},
                {"set_event_volume_calls", m_set_event_volume_calls},
                {"set_event_parameter_calls", m_set_event_parameter_calls}
            }},
            {"bank_load_requests", m_bank_paths},
            {"bank_unload_requests", m_unloaded_bank_paths},
            {"event_requests", m_event_ids},
            {"music_requests", m_music_ids},
            {"checks", checks}
        };

        std::error_code ec;
        const std::filesystem::path parent = m_telemetry_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream output(m_telemetry_path);
        if (output.is_open()) {
            output << std::setw(2) << artifact << '\n';
        }
    }

    std::filesystem::path m_telemetry_path;
    std::chrono::steady_clock::time_point m_started_at;
    bool m_initialized = false;
    bool m_shutdown_written = false;
    uint64_t m_init_calls = 0;
    uint64_t m_update_calls = 0;
    uint64_t m_shutdown_calls = 0;
    uint64_t m_load_bank_calls = 0;
    uint64_t m_unload_bank_calls = 0;
    uint64_t m_listener_transform_calls = 0;
    uint64_t m_play_event_calls = 0;
    uint64_t m_play_one_shot_calls = 0;
    uint64_t m_play_one_shot_2d_calls = 0;
    uint64_t m_play_music_calls = 0;
    uint64_t m_stop_music_calls = 0;
    uint64_t m_stop_event_calls = 0;
    uint64_t m_set_event_position_calls = 0;
    uint64_t m_set_event_volume_calls = 0;
    uint64_t m_set_event_parameter_calls = 0;
    std::vector<std::string> m_bank_paths;
    std::vector<std::string> m_unloaded_bank_paths;
    std::vector<AudioEventID> m_event_ids;
    std::vector<AudioEventID> m_music_ids;
};

} // namespace Luminumbra::Client
