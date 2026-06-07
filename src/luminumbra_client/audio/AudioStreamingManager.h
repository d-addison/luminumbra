#pragma once
#include "IAudioManager.h"
#include <miniaudio.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

namespace Luminumbra::Client {

// High-performance audio streaming with LOD and memory optimization
class AudioStreamingManager {
public:
    AudioStreamingManager();
    ~AudioStreamingManager();

    bool Initialize();
    void Shutdown();
    void Update(float deltaTime);

    // Audio quality levels
    enum class AudioQuality {
        Low,      // 22kHz, mono, high compression
        Medium,   // 44kHz, stereo, medium compression
        High,     // 48kHz, stereo, low compression
        Ultra     // 96kHz, stereo, lossless
    };

    // Streaming priorities
    enum class StreamPriority {
        Critical,   // Player sounds, UI, immediate vicinity
        High,       // Combat, important gameplay sounds
        Medium,     // Ambient, distant sounds
        Low,        // Far background sounds
        Deferred    // Pre-cached sounds for future use
    };

    struct StreamingSound {
        AudioEventHandle handle;
        ma_sound* sound = nullptr;
        AudioQuality quality = AudioQuality::Medium;
        StreamPriority priority = StreamPriority::Medium;
        glm::vec3 position = {0.0f, 0.0f, 0.0f};
        float distance_to_listener = 0.0f;
        bool is_looping = false;
        bool needs_quality_update = false;
        
        // Memory management
        std::unique_ptr<ma_decoder> decoder;
        std::vector<uint8_t> compressed_data;
        size_t memory_usage = 0;
        float last_access_time = 0.0f;
        bool is_resident_in_memory = false;
    };

    // Dynamic quality adjustment based on performance
    struct PerformanceSettings {
        float target_frame_time_ms = 16.67f; // 60 FPS target
        float audio_budget_percentage = 15.0f; // % of frame time for audio
        int max_simultaneous_streams = 64;
        int max_high_quality_streams = 16;
        size_t max_audio_memory_mb = 256;
        bool enable_dynamic_quality = true;
        bool enable_predictive_loading = true;
        float quality_downgrade_threshold_ms = 2.0f;
        float quality_upgrade_threshold_ms = 0.5f;
    };

    // Spatial audio LOD system
    struct SpatialLOD {
        float near_distance = 10.0f;    // Full quality
        float medium_distance = 50.0f;   // Reduced quality
        float far_distance = 100.0f;     // Minimum quality
        float cull_distance = 200.0f;    // Not played at all
        
        AudioQuality near_quality = AudioQuality::High;
        AudioQuality medium_quality = AudioQuality::Medium;
        AudioQuality far_quality = AudioQuality::Low;
        
        // Frequency-based culling for distant sounds
        float high_freq_cull_distance = 80.0f;
        float mid_freq_cull_distance = 120.0f;
    };

    // Audio compression and caching
    struct CompressionSettings {
        bool use_adaptive_compression = true;
        float compression_ratio_target = 0.3f; // 30% of original size
        bool cache_frequently_used = true;
        int cache_size_limit_mb = 128;
        bool use_audio_chunks = true; // Stream in chunks for large files
        size_t chunk_size_kb = 64;
    };

    // Public interface
    AudioEventHandle PlayStreamingSound(const AudioEventID& eventID, 
                                      const glm::vec3& position,
                                      StreamPriority priority = StreamPriority::Medium,
                                      AudioQuality quality = AudioQuality::Medium);

    void UpdateListenerPosition(const glm::vec3& position);
    void SetPerformanceSettings(const PerformanceSettings& settings);
    void SetSpatialLOD(const SpatialLOD& lod);
    void SetCompressionSettings(const CompressionSettings& settings);

    // Memory and performance monitoring
    struct StreamingMetrics {
        size_t total_memory_usage = 0;
        size_t compressed_memory_usage = 0;
        int active_streams = 0;
        int high_quality_streams = 0;
        float current_audio_load_ms = 0.0f;
        float avg_frame_audio_time_ms = 0.0f;
        int cache_hits = 0;
        int cache_misses = 0;
        int quality_downgrades = 0;
        int quality_upgrades = 0;
        int sounds_culled = 0;
    };

    const StreamingMetrics& GetMetrics() const { return m_metrics; }
    void ResetMetrics() { m_metrics = {}; }

    // Advanced features
    void PreloadSoundsForArea(const glm::vec3& center, float radius);
    void UnloadDistantSounds(const glm::vec3& listener_pos, float max_distance);
    void ForceLODUpdate(); // Immediate quality adjustment
    void SetAudioBudget(float max_time_per_frame_ms);

private:
    // Core streaming functionality
    void UpdateStreamQuality();
    void UpdateSpatialLOD();
    void ManageMemoryUsage();
    void ProcessLoadingQueue();
    
    // Quality management
    AudioQuality CalculateOptimalQuality(const StreamingSound& sound) const;
    bool ShouldUpgradeQuality(const StreamingSound& sound) const;
    bool ShouldDowngradeQuality(const StreamingSound& sound) const;
    void ApplyQualityChange(StreamingSound& sound, AudioQuality new_quality);
    
    // Compression and caching
    std::vector<uint8_t> CompressAudioData(const std::vector<uint8_t>& raw_data, float target_ratio);
    std::vector<uint8_t> DecompressAudioData(const std::vector<uint8_t>& compressed_data);
    bool LoadSoundIntoCache(const AudioEventID& eventID);
    void EvictLeastRecentlyUsed();
    
    // Spatial calculations
    float CalculateAudioImportance(const StreamingSound& sound) const;
    bool ShouldCullSound(const StreamingSound& sound) const;
    void UpdateSoundDistances();
    
    // Background loading system
    struct LoadingTask {
        AudioEventID eventID;
        StreamPriority priority;
        glm::vec3 expected_position;
        std::function<void(bool)> completion_callback;
    };

    std::queue<LoadingTask> m_loading_queue;
    std::thread m_loading_thread;
    std::mutex m_loading_mutex;
    std::condition_variable m_loading_cv;
    std::atomic<bool> m_shutdown{false};

    void LoadingThreadFunction();
    void ProcessLoadingTask(const LoadingTask& task);

    // Performance monitoring
    void UpdatePerformanceMetrics(float frame_audio_time_ms);
    void AdjustPerformanceSettings();

    // Member variables
    std::unordered_map<AudioEventHandle, std::unique_ptr<StreamingSound>> m_streaming_sounds;
    std::unordered_map<AudioEventID, std::vector<uint8_t>> m_audio_cache;
    std::unordered_map<AudioEventID, float> m_cache_access_times;

    glm::vec3 m_listener_position{0.0f, 0.0f, 0.0f};
    glm::vec3 m_predicted_listener_position{0.0f, 0.0f, 0.0f}; // For predictive loading
    
    PerformanceSettings m_performance_settings;
    SpatialLOD m_spatial_lod;
    CompressionSettings m_compression_settings;
    StreamingMetrics m_metrics;
    
    // Timing and performance
    float m_last_update_time = 0.0f;
    float m_quality_update_timer = 0.0f;
    const float QUALITY_UPDATE_INTERVAL = 0.1f; // Update every 100ms
    
    AudioEventHandle m_next_handle = 1;
    
    // Frame timing for performance management
    std::vector<float> m_recent_frame_times;
    const size_t MAX_FRAME_SAMPLES = 60;
    
    mutable std::mutex m_sounds_mutex;
    mutable std::mutex m_cache_mutex;
};

// Utility class for audio format conversion and optimization
class AudioFormatOptimizer {
public:
    struct OptimizationResult {
        std::vector<uint8_t> optimized_data;
        size_t original_size;
        size_t compressed_size;
        float compression_ratio;
        AudioStreamingManager::AudioQuality achieved_quality;
    };

    static OptimizationResult OptimizeForQuality(const std::vector<uint8_t>& input_data,
                                                AudioStreamingManager::AudioQuality target_quality,
                                                bool preserve_spatial_info = true);

    static OptimizationResult OptimizeForDistance(const std::vector<uint8_t>& input_data,
                                                 float distance,
                                                 const AudioStreamingManager::SpatialLOD& lod_settings);

    static bool ConvertSampleRate(std::vector<uint8_t>& audio_data, 
                                 int from_sample_rate, 
                                 int to_sample_rate);

private:
    static std::vector<uint8_t> ApplyLowPassFilter(const std::vector<uint8_t>& input, float cutoff_freq);
    static std::vector<uint8_t> ConvertToMono(const std::vector<uint8_t>& stereo_input);
    static float CalculateAudioComplexity(const std::vector<uint8_t>& audio_data);
};

} // namespace Luminumbra::Client