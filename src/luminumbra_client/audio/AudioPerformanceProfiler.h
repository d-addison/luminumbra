#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <fstream>

namespace Luminumbra::Client {

// Comprehensive audio performance monitoring and profiling system
class AudioPerformanceProfiler {
public:
    AudioPerformanceProfiler();
    ~AudioPerformanceProfiler();

    // Profiling scope management
    class ScopedTimer {
    public:
        ScopedTimer(AudioPerformanceProfiler* profiler, const std::string& name);
        ~ScopedTimer();
        
        void AddMetadata(const std::string& key, float value);
        void AddMetadata(const std::string& key, const std::string& value);
        
    private:
        AudioPerformanceProfiler* m_profiler;
        std::string m_name;
        std::chrono::high_resolution_clock::time_point m_start_time;
        std::unordered_map<std::string, std::string> m_metadata;
    };

    // Performance metrics categories
    struct AudioSystemMetrics {
        // Overall system performance
        float total_audio_cpu_percentage = 0.0f;
        float frame_audio_time_ms = 0.0f;
        float avg_frame_audio_time_ms = 0.0f;
        float peak_frame_audio_time_ms = 0.0f;
        
        // Memory usage
        size_t total_audio_memory_bytes = 0;
        size_t streaming_memory_bytes = 0;
        size_t cache_memory_bytes = 0;
        size_t reverb_memory_bytes = 0;
        int memory_allocations_per_frame = 0;
        
        // Sound management
        int active_sounds = 0;
        int streaming_sounds = 0;
        int cached_sounds = 0;
        int culled_sounds = 0;
        int quality_downgrades = 0;
        int quality_upgrades = 0;
        
        // 3D Audio processing
        int occlusion_calculations = 0;
        int reflection_calculations = 0;
        float avg_occlusion_time_us = 0.0f;
        float avg_reflection_time_us = 0.0f;
        
        // Streaming performance
        float streaming_bandwidth_mbps = 0.0f;
        int streaming_cache_hits = 0;
        int streaming_cache_misses = 0;
        float compression_ratio = 0.0f;
        
        // Reverb processing
        float reverb_cpu_percentage = 0.0f;
        int reverb_convolution_length = 0;
        float reverb_processing_time_ms = 0.0f;
        
        // Error tracking
        int audio_buffer_underruns = 0;
        int streaming_errors = 0;
        int initialization_failures = 0;
    };

    // Detailed timing breakdown
    struct ComponentTimings {
        float miniaudio_processing_ms = 0.0f;
        float occlusion_calculation_ms = 0.0f;
        float propagation_calculation_ms = 0.0f;
        float reverb_processing_ms = 0.0f;
        float streaming_management_ms = 0.0f;
        float environmental_update_ms = 0.0f;
        float spatial_positioning_ms = 0.0f;
        float quality_management_ms = 0.0f;
    };

    // Performance thresholds and warnings
    struct PerformanceThresholds {
        float max_frame_audio_time_ms = 5.0f;
        float warning_cpu_percentage = 80.0f;
        size_t max_memory_mb = 512;
        int max_active_sounds = 128;
        float max_streaming_bandwidth_mbps = 10.0f;
        
        // Quality degradation triggers
        float quality_downgrade_cpu_threshold = 70.0f;
        float quality_upgrade_cpu_threshold = 40.0f;
        size_t quality_downgrade_memory_threshold_mb = 400;
    };

    // Public interface
    std::unique_ptr<ScopedTimer> StartTimer(const std::string& name);
    void RecordEvent(const std::string& event_name, float value = 1.0f);
    void UpdateSystemMetrics(const AudioSystemMetrics& metrics);
    void UpdateComponentTimings(const ComponentTimings& timings);
    
    // Performance analysis
    AudioSystemMetrics GetCurrentMetrics() const;
    ComponentTimings GetCurrentTimings() const;
    std::vector<std::string> GetPerformanceWarnings() const;
    
    // Historical data and trending
    struct HistoricalData {
        std::vector<float> cpu_usage_history;
        std::vector<size_t> memory_usage_history;
        std::vector<int> active_sounds_history;
        std::vector<float> frame_time_history;
        std::chrono::steady_clock::time_point start_time;
        
        void AddSample(const AudioSystemMetrics& metrics);
        void TrimToSize(size_t max_samples = 3600); // 1 minute at 60fps
    };
    
    const HistoricalData& GetHistoricalData() const { return m_historical_data; }
    void ClearHistory() { m_historical_data = {}; }
    
    // Performance recommendations
    struct PerformanceRecommendation {
        enum class Type {
            ReduceActiveStreams,
            LowerAudioQuality,
            IncreaseCacheSize,
            ReduceReverbQuality,
            DisableDistantSounds,
            OptimizeOcclusion,
            ReduceReflectionDepth
        };
        
        Type type;
        std::string description;
        float estimated_improvement_percentage;
        int priority; // 1-10, higher is more important
    };
    
    std::vector<PerformanceRecommendation> GetPerformanceRecommendations() const;
    
    // Profiling controls
    void SetProfilingEnabled(bool enabled) { m_profiling_enabled = enabled; }
    bool IsProfilingEnabled() const { return m_profiling_enabled; }
    void SetDetailedProfiling(bool enabled) { m_detailed_profiling = enabled; }
    void SetThresholds(const PerformanceThresholds& thresholds) { m_thresholds = thresholds; }
    
    // Export and logging
    void ExportToCSV(const std::string& filename) const;
    void ExportToJSON(const std::string& filename) const;
    void StartLoggingToFile(const std::string& filename);
    void StopLoggingToFile();
    
    // Real-time monitoring
    void EnableRealtimeMonitoring(std::function<void(const AudioSystemMetrics&)> callback);
    void DisableRealtimeMonitoring();

private:
    struct TimingRecord {
        std::string name;
        std::chrono::high_resolution_clock::time_point start_time;
        std::chrono::high_resolution_clock::time_point end_time;
        std::unordered_map<std::string, std::string> metadata;
        float duration_ms = 0.0f;
    };

    struct EventRecord {
        std::string name;
        float value;
        std::chrono::steady_clock::time_point timestamp;
    };

    // Performance monitoring
    void AnalyzePerformanceBottlenecks();
    void CheckPerformanceThresholds();
    void UpdateMovingAverages();
    void GenerateWarnings();
    
    // Data management
    void TrimRecordsToSize(size_t max_records = 10000);
    void SaveRecordToFile(const TimingRecord& record);
    
    // Thread safety
    mutable std::mutex m_metrics_mutex;
    mutable std::mutex m_records_mutex;
    mutable std::mutex m_file_mutex;
    
    // Core data
    AudioSystemMetrics m_current_metrics;
    ComponentTimings m_current_timings;
    PerformanceThresholds m_thresholds;
    HistoricalData m_historical_data;
    
    // Profiling state
    bool m_profiling_enabled = true;
    bool m_detailed_profiling = false;
    
    // Records and analysis
    std::vector<TimingRecord> m_timing_records;
    std::vector<EventRecord> m_event_records;
    std::vector<std::string> m_current_warnings;
    std::vector<PerformanceRecommendation> m_current_recommendations;
    
    // Moving averages for smoothing
    struct MovingAverage {
        std::vector<float> samples;
        size_t max_samples = 60; // 1 second at 60fps
        float current_average = 0.0f;
        
        void AddSample(float value);
        float GetAverage() const { return current_average; }
    };
    
    std::unordered_map<std::string, MovingAverage> m_moving_averages;
    
    // File logging
    std::unique_ptr<std::ofstream> m_log_file;
    std::string m_log_filename;
    bool m_logging_enabled = false;
    
    // Real-time monitoring
    std::function<void(const AudioSystemMetrics&)> m_realtime_callback;
    bool m_realtime_monitoring_enabled = false;
    
    // Performance analysis
    std::chrono::steady_clock::time_point m_last_analysis_time;
    const float ANALYSIS_INTERVAL_SECONDS = 1.0f;
};

// Macro for easy profiling scope creation
#define AUDIO_PROFILE_SCOPE(profiler, name) \
    auto timer_##__LINE__ = profiler->StartTimer(name)

#define AUDIO_PROFILE_FUNCTION(profiler) \
    auto timer_##__LINE__ = profiler->StartTimer(__FUNCTION__)

// Performance monitoring utilities
class AudioPerformanceUtils {
public:
    // CPU usage monitoring
    static float GetCurrentCPUUsage();
    static float GetProcessCPUUsage();
    static float GetThreadCPUUsage();
    
    // Memory monitoring
    static size_t GetCurrentMemoryUsage();
    static size_t GetPeakMemoryUsage();
    static size_t GetAvailableMemory();
    
    // System capabilities
    static int GetOptimalBufferSize();
    static int GetRecommendedSampleRate();
    static int GetMaxSimultaneousStreams();
    static float GetSystemAudioLatency();
    
    // Performance benchmarking
    struct BenchmarkResult {
        float convolution_performance_score = 1.0f;
        float streaming_performance_score = 1.0f;
        float occlusion_performance_score = 1.0f;
        float overall_performance_score = 1.0f;
        std::string performance_class; // "Low", "Medium", "High", "Ultra"
    };
    
    static BenchmarkResult RunPerformanceBenchmark();
    
    // Adaptive quality recommendations
    static AudioPerformanceProfiler::PerformanceRecommendation::Type 
        GetRecommendedOptimization(const AudioPerformanceProfiler::AudioSystemMetrics& metrics);

private:
    static std::chrono::steady_clock::time_point s_last_cpu_measurement;
    static float s_cached_cpu_usage;
    static const float CPU_CACHE_DURATION_SECONDS;
};

// Integration helper for automatic performance monitoring
class AudioSystemIntegration {
public:
    AudioSystemIntegration(AudioPerformanceProfiler* profiler);
    ~AudioSystemIntegration();
    
    // Automatic metric collection hooks
    void HookIntoMiniaudio(ma_engine* engine);
    void HookIntoStreaming(class AudioStreamingManager* streaming_manager);
    void HookIntoReverb(class AdvancedReverbSystem* reverb_system);
    void HookIntoPropagation(class AudioPropagationSystem* propagation_system);
    
    // Frame-based monitoring
    void BeginFrame();
    void EndFrame();
    
    // Automatic threshold monitoring
    void EnableAutoThresholdAdjustment(bool enable) { m_auto_adjust_thresholds = enable; }

private:
    AudioPerformanceProfiler* m_profiler;
    bool m_auto_adjust_thresholds = true;
    std::chrono::high_resolution_clock::time_point m_frame_start_time;
    
    // Integration state
    std::unordered_map<void*, std::string> m_hooked_systems;
    
    void CollectSystemMetrics();
    void AdjustThresholdsBasedOnHardware();
};

} // namespace Luminumbra::Client