#pragma once
#include <miniaudio.h>
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <random>

namespace Luminumbra::Client {

// Advanced procedural sound generation system
class ProceduralSoundGenerator {
public:
    ProceduralSoundGenerator(int sample_rate = 48000);
    ~ProceduralSoundGenerator();

    // Sound generation types
    enum class GenerationType {
        Wind,
        Fire,
        Water,
        CreatureVocal,
        Machinery,
        CrystalResonance,
        Magic,
        Impact,
        Ambient
    };

    // Parameters for different generation types
    struct WindParameters {
        float intensity = 0.5f;           // 0.0 = calm, 1.0 = hurricane
        float frequency_base = 100.0f;    // Base frequency in Hz
        float turbulence = 0.3f;          // Amount of chaotic variation
        float directional_bias = 0.0f;    // -1.0 = left, 1.0 = right
        float temperature = 20.0f;        // Celsius, affects frequency content
        float obstacle_density = 0.0f;    // How much obstruction causes whistling
        glm::vec3 direction = {1.0f, 0.0f, 0.0f};
    };

    struct FireParameters {
        float intensity = 0.7f;           // 0.0 = ember, 1.0 = inferno
        float size = 1.0f;               // Size multiplier
        float fuel_type = 0.5f;          // 0.0 = dry wood, 1.0 = liquid fuel
        float oxygen_level = 1.0f;       // Affects crackling frequency
        float moisture = 0.1f;           // Wet fuel creates different sounds
        float wind_interaction = 0.0f;   // How much wind affects the fire
        bool enable_crackling = true;
        bool enable_roaring = true;
    };

    struct WaterParameters {
        float flow_rate = 0.5f;          // 0.0 = still, 1.0 = raging rapids
        float depth = 1.0f;              // Affects resonance
        float viscosity = 1.0f;          // 1.0 = water, higher = thicker
        float temperature = 20.0f;       // Affects bubble formation
        float turbulence = 0.3f;         // Amount of chaotic flow
        float surface_tension = 1.0f;    // Affects droplet sounds
        bool enable_bubbles = true;
        bool enable_splashing = false;
        std::string water_type = "fresh"; // fresh, salt, acidic
    };

    struct CreatureVocalParameters {
        float size = 1.0f;               // Size of creature (affects fundamental frequency)
        float aggression = 0.5f;         // 0.0 = passive, 1.0 = extremely aggressive
        float emotional_state = 0.5f;    // 0.0 = calm, 1.0 = excited/fearful
        float health = 1.0f;             // Affects vocal strength
        float age = 0.5f;                // Young vs old creature characteristics
        float intelligence = 0.5f;       // Affects complexity of vocalizations
        std::string creature_type = "mammal"; // mammal, bird, reptile, insect, fantasy
        bool enable_harmonics = true;
        float breath_noise = 0.2f;       // Amount of breathing/air noise
    };

    struct MachineryParameters {
        float rpm = 1000.0f;             // Rotations per minute
        float load = 0.5f;               // 0.0 = idle, 1.0 = maximum load
        float efficiency = 0.8f;         // Affects noise vs useful sound
        float age = 0.3f;                // Newer machines are quieter
        float maintenance = 0.8f;        // Well-maintained vs broken down
        std::string machine_type = "motor"; // motor, gears, hydraulic, steam
        bool enable_vibration = true;
        bool enable_electrical_hum = false;
        float lubrication = 0.7f;        // Affects friction sounds
    };

    struct CrystalResonanceParameters {
        float size = 1.0f;               // Crystal size affects fundamental frequency
        float purity = 0.8f;             // Pure crystals have cleaner tones
        float energy_level = 0.5f;       // Magical/electrical energy affects brightness
        float harmonic_complexity = 0.6f; // Number of harmonic overtones
        float resonance_decay = 0.7f;    // How long tones sustain
        glm::vec3 crystal_structure = {1.0f, 1.0f, 1.0f}; // Affects harmonic ratios
        bool enable_interference = true; // Multiple crystals interfering
        float magical_resonance = 0.0f;  // Fantasy magic affects
    };

    // Core generation interface
    std::vector<float> GenerateSound(GenerationType type, float duration_seconds, 
                                   const std::unordered_map<std::string, float>& params = {});
    
    // Real-time generation for streaming
    void GenerateRealtimeBuffer(GenerationType type, float* output_buffer, int num_samples,
                              const std::unordered_map<std::string, float>& params = {});
    
    // Specialized generators
    std::vector<float> GenerateWind(const WindParameters& params, float duration);
    std::vector<float> GenerateFire(const FireParameters& params, float duration);
    std::vector<float> GenerateWater(const WaterParameters& params, float duration);
    std::vector<float> GenerateCreatureVocal(const CreatureVocalParameters& params, float duration);
    std::vector<float> GenerateMachinery(const MachineryParameters& params, float duration);
    std::vector<float> GenerateCrystalResonance(const CrystalResonanceParameters& params, float duration);
    
    // Advanced procedural techniques
    std::vector<float> GenerateLayeredSound(const std::vector<GenerationType>& types,
                                          const std::vector<std::unordered_map<std::string, float>>& params_list,
                                          const std::vector<float>& layer_volumes,
                                          float duration);

    // Sound modulation and effects
    void ApplyFrequencyModulation(std::vector<float>& audio_data, float carrier_freq, float modulator_freq, float depth);
    void ApplyAmplitudeModulation(std::vector<float>& audio_data, float modulator_freq, float depth);
    void ApplyGranularSynthesis(std::vector<float>& audio_data, float grain_size_ms, float grain_density);
    void ApplySpectralFiltering(std::vector<float>& audio_data, const std::vector<float>& frequency_response);

    // Performance settings
    struct GenerationSettings {
        int quality_level = 2;           // 0 = low, 1 = medium, 2 = high, 3 = ultra
        bool enable_anti_aliasing = true;
        bool enable_noise_shaping = true;
        float cpu_budget_percentage = 5.0f; // Max CPU to use for generation
        int max_concurrent_generators = 4;
    };
    
    void SetSettings(const GenerationSettings& settings) { m_settings = settings; }
    const GenerationSettings& GetSettings() const { return m_settings; }

private:
    // Core synthesis components
    class OscillatorBank {
    public:
        OscillatorBank(int sample_rate, int max_oscillators = 32);
        void SetFrequency(int osc_index, float frequency);
        void SetAmplitude(int osc_index, float amplitude);
        void SetWaveform(int osc_index, int waveform_type); // 0=sine, 1=saw, 2=square, 3=noise
        void SetPhase(int osc_index, float phase);
        void GenerateBlock(float* output, int num_samples);
        void Reset();
        
    private:
        struct Oscillator {
            float frequency = 440.0f;
            float amplitude = 0.0f;
            float phase = 0.0f;
            int waveform_type = 0;
            bool active = false;
        };
        
        std::vector<Oscillator> m_oscillators;
        int m_sample_rate;
        float m_phase_increment_scale;
    };

    class NoiseGenerator {
    public:
        NoiseGenerator(int seed = 42);
        float GenerateWhiteNoise();
        float GeneratePinkNoise();
        float GenerateBrownNoise();
        void GeneratePerlinNoise(float* output, int num_samples, float frequency, float amplitude);
        
    private:
        std::mt19937 m_random_engine;
        std::uniform_real_distribution<float> m_distribution;
        float m_pink_noise_state[7] = {0};
        float m_brown_noise_state = 0.0f;
    };

    class FilterBank {
    public:
        FilterBank(int sample_rate);
        void ApplyLowpass(float* audio, int num_samples, float cutoff, float resonance = 1.0f);
        void ApplyHighpass(float* audio, int num_samples, float cutoff, float resonance = 1.0f);
        void ApplyBandpass(float* audio, int num_samples, float center_freq, float bandwidth);
        void ApplyFormantFilter(float* audio, int num_samples, const std::vector<float>& formant_freqs);
        
    private:
        struct BiquadFilter {
            float a0, a1, a2, b1, b2;
            float x1, x2, y1, y2;
            
            BiquadFilter() : a0(1), a1(0), a2(0), b1(0), b2(0), x1(0), x2(0), y1(0), y2(0) {}
            float Process(float input);
        };
        
        std::vector<BiquadFilter> m_filters;
        int m_sample_rate;
        
        void CalculateBiquadCoefficients(BiquadFilter& filter, int filter_type, 
                                       float frequency, float Q, float gain = 0.0f);
    };

    class EnvelopeGenerator {
    public:
        EnvelopeGenerator(int sample_rate);
        void SetADSR(float attack, float decay, float sustain, float release);
        void Trigger();
        void Release();
        float GetNextSample();
        bool IsActive() const { return m_stage != Stage::Idle; }
        
    private:
        enum class Stage { Idle, Attack, Decay, Sustain, Release };
        
        Stage m_stage = Stage::Idle;
        float m_current_level = 0.0f;
        float m_attack_rate, m_decay_rate, m_sustain_level, m_release_rate;
        int m_sample_rate;
    };

    // Specialized generation implementations
    void GenerateWindNoise(std::vector<float>& output, const WindParameters& params);
    void GenerateFireCrackling(std::vector<float>& output, const FireParameters& params);
    void GenerateWaterFlow(std::vector<float>& output, const WaterParameters& params);
    void GenerateVocalFormants(std::vector<float>& output, const CreatureVocalParameters& params);
    void GenerateMechanicalNoise(std::vector<float>& output, const MachineryParameters& params);
    void GenerateCrystalHarmonics(std::vector<float>& output, const CrystalResonanceParameters& params);
    
    // Utility functions
    float InterpolateParameter(float current, float target, float rate);
    std::vector<float> GenerateEnvelope(float attack, float decay, float sustain, float release, float duration);
    void NormalizeAudio(std::vector<float>& audio, float target_peak = 0.9f);
    void ApplyFadeInOut(std::vector<float>& audio, float fade_time_seconds);

    // Member variables
    int m_sample_rate;
    GenerationSettings m_settings;
    
    std::unique_ptr<OscillatorBank> m_oscillator_bank;
    std::unique_ptr<NoiseGenerator> m_noise_generator;
    std::unique_ptr<FilterBank> m_filter_bank;
    std::vector<std::unique_ptr<EnvelopeGenerator>> m_envelope_generators;
    
    // Performance monitoring
    mutable std::chrono::steady_clock::time_point m_last_generation_time;
    mutable float m_avg_generation_time_ms = 0.0f;
    mutable int m_active_generators = 0;
};

// Utility functions for parameter mapping
namespace ProceduralSoundUtils {
    // Convert game parameters to synthesis parameters
    ProceduralSoundGenerator::WindParameters MapWindParameters(float strength, const glm::vec3& direction, 
                                                               float temperature, const std::string& environment);
    
    ProceduralSoundGenerator::FireParameters MapFireParameters(float intensity, float size, 
                                                               const std::string& fuel_type, float wind_strength);
    
    ProceduralSoundGenerator::CreatureVocalParameters MapCreatureParameters(const std::string& species, 
                                                                            float size, float health, 
                                                                            float aggression, float age);
    
    // Preset generation for common scenarios
    std::unordered_map<std::string, float> GetWindPreset(const std::string& preset_name); // "gentle_breeze", "storm", "hurricane"
    std::unordered_map<std::string, float> GetFirePreset(const std::string& preset_name); // "campfire", "forge", "wildfire"
    std::unordered_map<std::string, float> GetWaterPreset(const std::string& preset_name); // "stream", "rapids", "waterfall"
    
    // Parameter interpolation for smooth transitions
    std::unordered_map<std::string, float> InterpolateParameters(
        const std::unordered_map<std::string, float>& from,
        const std::unordered_map<std::string, float>& to,
        float t);
}

} // namespace Luminumbra::Client