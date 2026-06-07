#pragma once
#include "MiniaudioManager.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>

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
    
    EnvironmentalAnalysis AnalyzeEnvironment(const glm::vec3& position, float analysis_radius = 20.0f);
    
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
    
    void SetSettings(const PropagationSettings& settings) { m_settings = settings; }
    const PropagationSettings& GetSettings() const { return m_settings; }
    
    // Performance monitoring
    struct PerformanceMetrics {
        float last_calculation_time_ms = 0.0f;
        int active_calculations = 0;
        int completed_calculations = 0;
        float avg_calculation_time_ms = 0.0f;
        int cache_hits = 0;
        int cache_misses = 0;
    };
    
    const PerformanceMetrics& GetMetrics() const { return m_metrics; }
    void ResetMetrics() { m_metrics = {}; }

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
        
        uint64_t HashPositions(const glm::vec3& source, const glm::vec3& listener, float tolerance = 0.5f);
        void CleanupOldEntries();
    };

    // Core calculation methods
    AudioPropagationPath CalculateDirectPath(const glm::vec3& source, const glm::vec3& listener);
    std::vector<AudioPropagationPath> CalculateReflectionPaths(const glm::vec3& source, 
                                                              const glm::vec3& listener,
                                                              int max_bounces);
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
}

} // namespace Luminumbra::Client