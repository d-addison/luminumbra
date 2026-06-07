#pragma once
#include <miniaudio.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <complex>
#include <unordered_map>
#include <string>

namespace Luminumbra::Client {

// Real-time convolution reverb system with environmental adaptation
class AdvancedReverbSystem {
public:
    AdvancedReverbSystem();
    ~AdvancedReverbSystem();

    bool Initialize(int sample_rate = 48000, int buffer_size = 512);
    void Shutdown();
    
    // Environment-specific impulse responses
    enum class ReverbEnvironment {
        SmallRoom,
        LargeRoom,
        Hall,
        Cathedral,
        Cave,
        Canyon,
        Forest,
        Underwater,
        CustomIR  // User-provided impulse response
    };

    // Reverb quality settings
    enum class ReverbQuality {
        Low,     // 256 samples, basic filtering
        Medium,  // 512 samples, moderate filtering
        High,    // 1024 samples, full processing
        Ultra    // 2048+ samples, maximum quality
    };

    struct ReverbParameters {
        float wet_level = 0.3f;        // Reverb mix level
        float dry_level = 0.7f;        // Direct signal level
        float pre_delay = 20.0f;       // Pre-delay in ms
        float decay_time = 1.2f;       // RT60 in seconds
        float room_size = 0.5f;        // 0.0 = small, 1.0 = huge
        float diffusion = 0.7f;        // Sound scattering
        float density = 0.8f;          // Early reflection density
        float damping = 0.4f;          // High-frequency absorption
        float modulation_rate = 0.2f;  // Chorus effect rate
        float modulation_depth = 0.1f; // Chorus effect depth
        
        // Advanced parameters
        float early_late_mix = 0.5f;   // Early vs late reflections
        float bass_boost = 0.0f;       // Low-frequency enhancement
        float treble_cut = 0.0f;       // High-frequency reduction
        glm::vec3 room_dimensions = {10.0f, 4.0f, 8.0f}; // For algorithmic reverb
    };

    // Convolution engine with real-time processing
    class ConvolutionEngine {
    public:
        ConvolutionEngine(int sample_rate, int buffer_size);
        ~ConvolutionEngine();
        
        bool LoadImpulseResponse(const std::vector<float>& impulse_response);
        bool LoadImpulseResponseFromFile(const std::string& file_path);
        void ProcessBuffer(const float* input, float* output, int num_samples);
        void SetWetDryMix(float wet, float dry);
        
        // Performance optimization
        void SetQuality(ReverbQuality quality);
        void UpdateConvolutionLength(int new_length);
        
    private:
        void InitializeFFT();
        void ConvolveFFT(const float* input, float* output, int num_samples);
        void ConvolveTimeDomain(const float* input, float* output, int num_samples);
        
        int m_sample_rate;
        int m_buffer_size;
        int m_ir_length = 0;
        ReverbQuality m_quality = ReverbQuality::Medium;
        
        std::vector<float> m_impulse_response;
        std::vector<std::complex<float>> m_ir_fft;
        std::vector<float> m_overlap_buffer;
        
        // FFT workspace
        std::vector<std::complex<float>> m_fft_input;
        std::vector<std::complex<float>> m_fft_output;
        
        float m_wet_gain = 0.3f;
        float m_dry_gain = 0.7f;
    };

    // Procedural impulse response generation
    class ProceduralIR {
    public:
        static std::vector<float> GenerateRoomIR(const ReverbParameters& params, int sample_rate);
        static std::vector<float> GenerateCaveIR(float cave_size, float absorption, int sample_rate);
        static std::vector<float> GenerateForestIR(float density, float tree_absorption, int sample_rate);
        static std::vector<float> GenerateCanyonIR(float width, float height, float echo_decay, int sample_rate);
        
    private:
        static std::vector<float> GenerateEarlyReflections(const ReverbParameters& params, int sample_rate);
        static std::vector<float> GenerateLateReverb(const ReverbParameters& params, int sample_rate);
        static void ApplyFrequencyShaping(std::vector<float>& ir, const ReverbParameters& params, int sample_rate);
    };

    // Multi-tap delay for early reflections
    class MultiTapDelay {
    public:
        struct TapSettings {
            float delay_ms;
            float gain;
            float feedback;
            bool enable_modulation;
        };
        
        MultiTapDelay(int sample_rate, int max_delay_ms = 500);
        void SetTaps(const std::vector<TapSettings>& taps);
        void Process(const float* input, float* output, int num_samples);
        void Clear();
        
    private:
        int m_sample_rate;
        int m_buffer_size;
        std::vector<float> m_delay_buffer;
        std::vector<TapSettings> m_taps;
        int m_write_index = 0;
    };

    // Environmental adaptation system
    class EnvironmentalAdapter {
    public:
        struct EnvironmentAnalysis {
            float estimated_volume = 100.0f;    // m³
            float surface_area = 100.0f;        // m²
            float avg_absorption = 0.3f;
            bool is_enclosed = true;
            float ceiling_height = 3.0f;
            glm::vec3 dominant_dimension = {10.0f, 3.0f, 8.0f};
        };
        
        ReverbParameters AdaptToEnvironment(const EnvironmentAnalysis& env);
        void UpdateRealTime(const glm::vec3& listener_pos, 
                          const std::vector<glm::vec3>& reflection_points);
        
    private:
        float CalculateRT60(float volume, float surface_area, float absorption);
        float EstimateRoomDiffusion(const std::vector<glm::vec3>& reflection_points);
    };

    // Public interface
    void SetEnvironment(ReverbEnvironment env);
    void SetCustomParameters(const ReverbParameters& params);
    void SetQuality(ReverbQuality quality);
    void ProcessAudio(const float* input, float* output, int num_samples, int num_channels = 2);
    
    // Real-time environment adaptation
    void UpdateEnvironmentalData(const glm::vec3& listener_pos,
                               const std::vector<glm::vec3>& reflection_points,
                               float room_volume,
                               float surface_absorption);
    
    // Performance and resource management
    void SetCPUBudget(float max_cpu_percentage);
    void EnableAdaptiveQuality(bool enable);
    
    struct ReverbMetrics {
        float cpu_usage_percentage = 0.0f;
        float processing_time_ms = 0.0f;
        int active_taps = 0;
        int convolution_length = 0;
        ReverbQuality current_quality = ReverbQuality::Medium;
        bool is_real_time = true;
    };
    
    const ReverbMetrics& GetMetrics() const { return m_metrics; }

private:
    void InitializeEnvironmentPresets();
    void UpdateConvolutionEngine();
    void ProcessEarlyReflections(const float* input, float* output, int num_samples);
    void ProcessLateReverb(const float* input, float* output, int num_samples);
    void MixToOutput(const float* early_reflections, const float* late_reverb, 
                    const float* dry_input, float* output, int num_samples);
    
    // Performance monitoring
    void UpdatePerformanceMetrics(float processing_time_ms);
    void AdaptQualityBasedOnPerformance();

    int m_sample_rate = 48000;
    int m_buffer_size = 512;
    
    std::unique_ptr<ConvolutionEngine> m_convolution_engine;
    std::unique_ptr<MultiTapDelay> m_early_reflections;
    std::unique_ptr<EnvironmentalAdapter> m_env_adapter;
    
    ReverbEnvironment m_current_environment = ReverbEnvironment::SmallRoom;
    ReverbParameters m_current_params;
    ReverbQuality m_quality = ReverbQuality::Medium;
    
    // Environment presets
    std::unordered_map<ReverbEnvironment, ReverbParameters> m_environment_presets;
    
    // Performance management
    float m_cpu_budget_percentage = 10.0f;
    bool m_adaptive_quality_enabled = true;
    ReverbMetrics m_metrics;
    
    // Processing buffers
    std::vector<float> m_early_reflection_buffer;
    std::vector<float> m_late_reverb_buffer;
    std::vector<float> m_temp_buffer;
    
    // Real-time adaptation
    float m_adaptation_rate = 0.1f; // How fast to adapt to changes
    float m_last_adaptation_time = 0.0f;
    bool m_needs_ir_update = false;
};

// Impulse Response Library Manager
class IRLibraryManager {
public:
    IRLibraryManager();
    ~IRLibraryManager();

    bool LoadIRLibrary(const std::string& library_path);
    std::vector<std::string> GetAvailableIRs() const;
    std::vector<float> GetIR(const std::string& name) const;
    
    // IR metadata
    struct IRMetadata {
        std::string name;
        std::string environment_type;
        float duration_seconds;
        int sample_rate;
        std::string description;
        glm::vec3 recording_position;
        float room_volume;
        std::vector<std::string> tags;
    };
    
    IRMetadata GetIRMetadata(const std::string& name) const;
    std::vector<std::string> SearchIRs(const std::string& environment_type, 
                                     const std::vector<std::string>& tags = {}) const;

private:
    std::unordered_map<std::string, std::vector<float>> m_impulse_responses;
    std::unordered_map<std::string, IRMetadata> m_ir_metadata;
    
    bool LoadWAVFile(const std::string& file_path, std::vector<float>& output, int& sample_rate);
    void ProcessIRForOptimalUse(std::vector<float>& ir, int sample_rate);
};

// Frequency-dependent reverb processor
class FrequencyDependentReverb {
public:
    FrequencyDependentReverb(int sample_rate, int num_bands = 8);
    ~FrequencyDependentReverb();
    
    void SetBandParameters(int band_index, float decay_time, float gain);
    void Process(const float* input, float* output, int num_samples);
    void Reset();
    
private:
    struct BandProcessor {
        float decay_time = 1.0f;
        float gain = 1.0f;
        float current_energy = 0.0f;
        std::vector<float> delay_line;
        int delay_write_pos = 0;
    };
    
    void InitializeCrossovers();
    void ProcessBand(BandProcessor& band, const float* input, float* output, int num_samples);
    
    int m_sample_rate;
    int m_num_bands;
    std::vector<BandProcessor> m_bands;
    
    // Crossover filters for band splitting
    std::vector<float> m_crossover_frequencies;
    // Simplified - would need proper filter implementations
};

} // namespace Luminumbra::Client