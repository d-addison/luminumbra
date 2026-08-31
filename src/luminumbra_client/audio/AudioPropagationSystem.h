#pragma once
#include "MiniaudioManager.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include <atomic>
#include <condition_variable>
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Client {

struct AudioPropagationPath {
    std::vector<glm::vec3> points;
    float total_distance = 0.0f;
    float absorption_factor = 0.0f;
    float delay_ms = 0.0f;
    float volume_multiplier = 1.0f;
    int bounces = 0;
    bool is_direct_path = true;
};

struct AudioPropagationResult {
    AudioPropagationPath direct_path;
    std::vector<AudioPropagationPath> reflection_paths;
    float total_occlusion = 0.0f;
    float reverb_time = 0.0f;
    float early_reflections_delay = 0.0f;
    glm::vec3 dominant_reflection_direction = {0.0f, 0.0f, 0.0f};
};

class AudioPropagationSystem {
public:
    AudioPropagationSystem(Luminumbra::Systems::PhysicsSystem* physicsSystem);
    ~AudioPropagationSystem();

    // Core propagation calculation
    AudioPropagationResult CalculatePropagation(const glm::vec3& source,
                                                const glm::vec3& listener,
                                                float max_distance = 100.0f,
                                                int max_reflections = 5);

    // THIN thunder cue for a lightning strike (regression review). A strike
    // is a deterministic SIM world event; thunder is the delayed positional one-shot
    // it cues. This is a LIGHTWEIGHT additive hook (NOT new propagation machinery):
    // it returns the speed-of-sound delay (distance / speed_of_sound, the physically
    // correct flash-to-bang gap) and a distance attenuation for the strike, reusing
    // the existing direct-path geometry + the m_settings.speed_of_sound. The caller
    // schedules an IAudioManager::PlayOneShot at delay_seconds. The visual lightning
    // gate does NOT depend on this : it is a pure audio convenience.
    struct ThunderCue {
        float delay_seconds = 0.0f;     // distance / speed_of_sound (flash-to-bang)
        float distance = 0.0f;          // metres source->listener
        float volume_multiplier = 1.0f; // [0,1] distance/absorption attenuation
    };
    ThunderCue ComputeThunderCue(const glm::vec3& strike_position, const glm::vec3& listener) const;

    //  (AU1): atmosphere AMBIENCE bed on the propagation system. Wind/rain
    // ambience are non-positional weather beds (they envelop the listener), so this
    // is a LIGHTWEIGHT additive hook, NOT new ray-traced propagation: given a
    // requested ambience volume [0, 1] it returns the volume the listener actually
    // hears once the environment occludes/encloses it (open sky -> full weather
    // bed; an enclosed cave muffles the outdoor wind/rain). It reuses the existing
    // AnalyzeEnvironment enclosure estimate; no reflections, no physics raycast on
    // the call path unless a physics system is present. The EnvironmentalAudioSystem
    // drives the ambience layers through this so the weather bed responds to the
    // listener's space the same way other audio does. Null-audio safe (pure math).
    struct AmbienceBed {
        float requested_volume = 0.0f; // [0, 1] weather-driven ambience volume in
        float volume = 0.0f;           // [0, 1] occlusion/enclosure-adjusted output
        float openness = 1.0f;         // [0, 1] 1 = open sky, 0 = fully enclosed
    };
    AmbienceBed ComputeAmbienceBed(const glm::vec3& listener,
                                   float requested_volume,
                                   float analysis_radius = 20.0f);

    // distance-attenuated waterfall ROAR. A waterfall is a fixed
    // world site (WaterfallDetect); the roar is the positional ambient loop the
    // listener hears from it. Like ComputeThunderCue this is a LIGHTWEIGHT
    // additive hook (NOT new propagation machinery): given the fall's crest
    // position, its drop height (louder, deeper falls roar more) and the
    // listener, it returns a distance-attenuated loop volume + a low-pass /
    // muffle factor that rises with distance (the high-frequency hiss falls off
    // faster than the low rumble). Reuses the existing direct-path distance
    // attenuation; no reflection tracing, no physics raycast required. The
    // visual WaterfallVisual gate does NOT depend on this (audio is optional
    // dressing). Render-only: nothing here writes back to sim/world_hash.
    struct WaterfallRoar {
        float distance = 0.0f; // metres crest->listener
        float volume = 0.0f;   // [0,1] distance + drop-scaled loop volume
        float low_pass = 0.0f; // [0,1] 0 = bright/near, 1 = muffled/far
        bool audible = false;  // volume above the silence floor
    };
    // STATIC: the roar is pure math over its arguments — no propagation
    // state, no physics — so live wiring can call it without constructing the
    // full system (whose ctor spawns worker threads).
    static WaterfallRoar ComputeWaterfallRoar(const glm::vec3& crest_position,
                                              float drop_height,
                                              const glm::vec3& listener,
                                              float max_distance = 400.0f);

    // Async propagation for performance
    void CalculatePropagationAsync(const glm::vec3& source,
                                   const glm::vec3& listener,
                                   std::function<void(AudioPropagationResult)> callback,
                                   float max_distance = 100.0f);

    // Volume-based occlusion for large environments
    AudioPropagationResult CalculateVolumetricPropagation(const glm::vec3& source,
                                                          const glm::vec3& listener,
                                                          float volume_resolution = 1.0f);

    // Environmental analysis
    struct EnvironmentalAnalysis {
        float room_volume = 0.0f;
        float surface_area = 0.0f;
        float avg_absorption = 0.3f;
        float rt60_estimate = 1.0f; // Reverberation time
        bool is_enclosed = false;
        glm::vec3 room_center = {0.0f, 0.0f, 0.0f};
    };

    EnvironmentalAnalysis AnalyzeEnvironment(const glm::vec3& position,
                                             float analysis_radius = 20.0f);

    // Performance settings
    struct PropagationSettings {
        int max_ray_samples = 64;
        int max_reflection_depth = 5;
        float min_reflection_energy = 0.01f;
        float speed_of_sound = 343.0f; // m/s
        bool enable_diffraction = true;
        bool enable_scattering = false;
        float computation_budget_ms = 2.0f; // Max time per frame
    };

    void SetSettings(const PropagationSettings& settings) {
        m_settings = settings;
    }
    const PropagationSettings& GetSettings() const {
        return m_settings;
    }

    // Performance monitoring
    struct PerformanceMetrics {
        float last_calculation_time_ms = 0.0f;
        int active_calculations = 0;
        int completed_calculations = 0;
        float avg_calculation_time_ms = 0.0f;
        int cache_hits = 0;
        int cache_misses = 0;
    };

    const PerformanceMetrics& GetMetrics() const {
        return m_metrics;
    }
    void ResetMetrics() {
        m_metrics = {};
    }

private:
    struct PropagationCache {
        struct CacheEntry {
            AudioPropagationResult result;
            std::chrono::steady_clock::time_point timestamp;
            glm::vec3 source_pos;
            glm::vec3 listener_pos;
            float hash_tolerance = 0.5f;
        };

        std::unordered_map<uint64_t, CacheEntry> entries;
        std::mutex cache_mutex;
        float max_age_seconds = 1.0f;
        size_t max_entries = 1000;

        uint64_t
        HashPositions(const glm::vec3& source, const glm::vec3& listener, float tolerance = 0.5f);
        void CleanupOldEntries();
    };

    // Core calculation methods
    AudioPropagationPath CalculateDirectPath(const glm::vec3& source, const glm::vec3& listener);
    std::vector<AudioPropagationPath>
    CalculateReflectionPaths(const glm::vec3& source, const glm::vec3& listener, int max_bounces);
    AudioPropagationPath TraceReflectionPath(glm::vec3 current_pos,
                                             glm::vec3 direction,
                                             const glm::vec3& target,
                                             int remaining_bounces,
                                             float accumulated_absorption = 0.0f);

    // Advanced techniques
    std::vector<AudioPropagationPath> CalculateDiffractionPaths(const glm::vec3& source,
                                                                const glm::vec3& listener);
    float CalculateFrequencyDependentAbsorption(float base_absorption, float frequency_hz);
    glm::vec3 CalculateScatteredDirection(const glm::vec3& incident,
                                          const glm::vec3& normal,
                                          float scattering_coefficient);

    // Environmental analysis helpers
    float EstimateRoomVolume(const glm::vec3& center, float radius);
    float CalculateRT60(float room_volume, float surface_area, float avg_absorption);
    bool IsPointInEnclosedSpace(const glm::vec3& point);

    // Performance optimization
    bool ShouldUseCachedResult(const glm::vec3& source, const glm::vec3& listener);
    void UpdatePerformanceMetrics(float calculation_time_ms, bool cache_hit);

    Luminumbra::Systems::PhysicsSystem* m_physicsSystem;
    PropagationSettings m_settings;
    PropagationCache m_cache;
    mutable PerformanceMetrics m_metrics;

    // Async computation
    std::vector<std::thread> m_worker_threads;
    std::atomic<bool> m_shutdown{false};
    std::mutex m_task_mutex;
    std::condition_variable m_task_cv;

    struct AsyncTask {
        glm::vec3 source;
        glm::vec3 listener;
        float max_distance;
        std::function<void(AudioPropagationResult)> callback;
    };
    std::vector<AsyncTask> m_pending_tasks;

    void WorkerThreadFunction();
    void ProcessAsyncTasks();
};

// Utility functions for audio propagation
namespace AudioPropagationUtils {
float DbToLinear(float db);
float LinearToDb(float linear);
float CalculateAirAbsorption(float distance, float frequency_hz, float humidity = 50.0f);
float FrequencyToWavelength(float frequency_hz, float speed_of_sound = 343.0f);
bool IsFrequencyAudible(float frequency_hz);

// Psychoacoustic modeling
float CalculateLoudness(float amplitude, float frequency_hz);
float CalculateMasking(float signal_freq, float masker_freq, float masker_level);
} // namespace AudioPropagationUtils

} // namespace Luminumbra::Client