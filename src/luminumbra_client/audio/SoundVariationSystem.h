#pragma once
#include <unordered_map>
#include <vector>
#include <memory>
#include <random>
#include <functional>
#include <chrono>
#include <glm/glm.hpp>

namespace Luminumbra::Client {

class SoundVariationSystem {
public:
    SoundVariationSystem(int seed = 42);
    ~SoundVariationSystem() = default;

    // Core variation types
    enum class VariationType {
        None,
        Random,           // Random selection from pool
        Sequential,       // Play in sequence
        Weighted,         // Weighted random selection
        Contextual,       // Based on game context
        Procedural,       // Algorithmically generated
        Layered,          // Multiple sounds layered
        Responsive,       // React to environment/events
        Evolutionary      // Learns from player preferences
    };

    // Variation parameters for different sound categories
    struct VariationParameters {
        // Basic variation
        float pitch_variation = 0.1f;        // ±10% pitch variation
        float volume_variation = 0.05f;       // ±5% volume variation
        float timing_variation = 0.02f;       // ±2% timing variation
        
        // Advanced variation
        float formant_shift = 0.0f;          // Voice-like formant shifting
        float spectral_tilt = 0.0f;          // High-frequency emphasis/de-emphasis
        float harmonic_distortion = 0.0f;    // Subtle harmonic coloration
        float reverb_variation = 0.0f;       // Reverb amount variation
        
        // Contextual modulation
        bool enable_distance_variation = true;
        bool enable_material_variation = true;
        bool enable_weather_variation = false;
        bool enable_time_variation = false;
        
        // Repetition avoidance
        int avoid_recent_count = 3;          // Don't repeat last N variations
        float repetition_penalty = 0.5f;     // Reduce probability of recent sounds
        
        // Learning parameters
        bool enable_adaptive_selection = false;
        float learning_rate = 0.01f;         // How quickly to adapt to preferences
    };

    // Context information for intelligent variation selection
    struct VariationContext {
        glm::vec3 world_position = {0.0f, 0.0f, 0.0f};
        glm::vec3 listener_position = {0.0f, 0.0f, 0.0f};
        std::string surface_material = "default";
        std::string environment_type = "outdoor";
        float time_of_day = 12.0f;           // 0-24 hours
        std::string weather_condition = "clear";
        float ambient_noise_level = 0.3f;
        float reverb_intensity = 0.2f;
        
        // Player/gameplay context
        float player_health = 1.0f;
        float stress_level = 0.0f;
        std::string activity_type = "idle";   // idle, walking, running, combat, crafting
        float activity_intensity = 0.0f;
        
        // Sound history
        std::vector<std::string> recently_played_variations;
        std::chrono::steady_clock::time_point last_played_time;
    };

    // Sound variation definition
    struct SoundVariation {
        std::string id;
        std::vector<std::string> file_paths;
        VariationType type = VariationType::Random;
        float weight = 1.0f;                 // For weighted selection
        VariationParameters parameters;
        
        // Contextual triggers
        std::vector<std::string> required_materials;
        std::vector<std::string> required_environments;
        std::pair<float, float> time_range = {0.0f, 24.0f};
        std::pair<float, float> distance_range = {0.0f, 1000.0f};
        std::vector<std::string> weather_conditions;
        
        // Layering information
        std::vector<std::string> layer_sounds;   // Additional sounds to layer
        std::vector<float> layer_volumes;        // Volume for each layer
        std::vector<float> layer_delays;         // Delay for each layer
        
        // Procedural generation parameters
        std::unordered_map<std::string, float> procedural_params;
        
        // Performance metrics (for learning)
        mutable float usage_count = 0.0f;
        mutable float positive_feedback = 0.0f;
        mutable float negative_feedback = 0.0f;
    };

    // Sound group - collection of related variations
    struct SoundGroup {
        std::string group_id;
        std::string description;
        std::vector<SoundVariation> variations;
        VariationParameters default_parameters;
        VariationType default_selection_strategy = VariationType::Random;
        
        // Group-level constraints
        float max_simultaneous_instances = 3.0f;
        float min_time_between_instances = 0.1f;
        bool allow_overlapping = true;
        
        // Adaptive learning
        bool enable_group_learning = false;
        std::unordered_map<std::string, float> variation_preferences;
    };

    // Main interface
    void RegisterSoundGroup(const SoundGroup& group);
    void UpdateSoundGroup(const std::string& group_id, const SoundGroup& group);
    void RemoveSoundGroup(const std::string& group_id);
    
    // Variation selection
    std::string SelectVariation(const std::string& group_id, const VariationContext& context);
    std::vector<std::string> SelectLayeredVariations(const std::string& group_id, const VariationContext& context);
    
    // Apply variation effects to sound parameters
    struct VariationResult {
        std::string selected_variation_id;
        std::vector<std::string> file_paths;
        float final_pitch = 1.0f;
        float final_volume = 1.0f;
        float timing_offset = 0.0f;
        std::unordered_map<std::string, float> effect_parameters;
        std::vector<std::string> layer_files;
        std::vector<float> layer_volumes;
        std::vector<float> layer_delays;
    };
    
    VariationResult ApplyVariation(const std::string& group_id, const std::string& variation_id, 
                                   const VariationContext& context);
    
    // Learning and adaptation
    void ProvideFeedback(const std::string& group_id, const std::string& variation_id, 
                        float feedback_score); // -1.0 to 1.0
    void UpdateUsageStatistics(const std::string& group_id, const std::string& variation_id);
    
    // Context-aware utilities
    float CalculateContextualWeight(const SoundVariation& variation, const VariationContext& context) const;
    bool IsVariationValid(const SoundVariation& variation, const VariationContext& context) const;
    
    // Performance and debugging
    struct SystemMetrics {
        int total_registered_groups = 0;
        int total_variations = 0;
        int selections_this_frame = 0;
        float average_selection_time_ms = 0.0f;
        int cache_hits = 0;
        int cache_misses = 0;
        size_t memory_usage_bytes = 0;
    };
    
    SystemMetrics GetMetrics() const;
    void ResetMetrics();
    
    // Configuration
    struct SystemSettings {
        bool enable_caching = true;
        int max_cache_size = 1000;
        bool enable_learning = true;
        float global_variation_intensity = 1.0f;
        int max_concurrent_selections = 64;
        bool enable_performance_scaling = true;
    };
    
    void SetSettings(const SystemSettings& settings) { m_settings = settings; }
    const SystemSettings& GetSettings() const { return m_settings; }

private:
    // Internal data structures
    std::unordered_map<std::string, SoundGroup> m_sound_groups;
    SystemSettings m_settings;
    
    // Random number generation
    std::mt19937 m_rng;
    std::uniform_real_distribution<float> m_uniform_dist;
    
    // Caching system
    struct CacheEntry {
        std::string result;
        VariationContext context;
        std::chrono::steady_clock::time_point timestamp;
    };
    mutable std::unordered_map<std::string, CacheEntry> m_selection_cache;
    
    // Performance tracking
    mutable SystemMetrics m_metrics;
    mutable std::chrono::steady_clock::time_point m_last_metrics_update;
    
    // Learning system
    struct LearningData {
        std::unordered_map<std::string, float> variation_scores;
        std::vector<std::pair<VariationContext, std::string>> training_data;
        int update_counter = 0;
    };
    std::unordered_map<std::string, LearningData> m_learning_data;
    
    // Internal methods
    std::string SelectRandomVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectWeightedVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectSequentialVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectContextualVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectProceduralVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectResponsiveVariation(const SoundGroup& group, const VariationContext& context);
    std::string SelectEvolutionaryVariation(const SoundGroup& group, const VariationContext& context);
    
    // Variation application
    void ApplyPitchVariation(VariationResult& result, const VariationParameters& params, const VariationContext& context);
    void ApplyVolumeVariation(VariationResult& result, const VariationParameters& params, const VariationContext& context);
    void ApplyTimingVariation(VariationResult& result, const VariationParameters& params, const VariationContext& context);
    void ApplySpectralVariation(VariationResult& result, const VariationParameters& params, const VariationContext& context);
    void ApplyContextualVariation(VariationResult& result, const VariationParameters& params, const VariationContext& context);
    
    // Context analysis
    float CalculateMaterialCompatibility(const std::string& material, const std::vector<std::string>& required_materials) const;
    float CalculateEnvironmentCompatibility(const std::string& environment, const std::vector<std::string>& required_environments) const;
    float CalculateTimeCompatibility(float current_time, const std::pair<float, float>& time_range) const;
    float CalculateDistanceCompatibility(float distance, const std::pair<float, float>& distance_range) const;
    
    // Learning algorithms
    void UpdateLearningModel(const std::string& group_id);
    float PredictVariationScore(const std::string& group_id, const std::string& variation_id, const VariationContext& context) const;
    
    // Caching
    std::string GetCacheKey(const std::string& group_id, const VariationContext& context) const;
    void UpdateCache(const std::string& cache_key, const std::string& result, const VariationContext& context) const;
    void CleanupCache() const;
    
    // Utility functions
    float Lerp(float a, float b, float t) const { return a + t * (b - a); }
    float Clamp(float value, float min_val, float max_val) const { 
        return std::max(min_val, std::min(max_val, value)); 
    }
    
    // Sequence tracking for sequential variation
    mutable std::unordered_map<std::string, int> m_sequence_indices;
    
    // Recent variation tracking for repetition avoidance
    mutable std::unordered_map<std::string, std::vector<std::string>> m_recent_variations;
    
    // Performance optimization
    void OptimizeForPerformance();
    bool ShouldSkipExpensiveCalculation() const;
};

// Utility functions for common sound variation scenarios
namespace SoundVariationUtils {
    
    // Create common variation patterns
    SoundVariationSystem::SoundGroup CreateFootstepVariations(const std::vector<std::string>& materials);
    SoundVariationSystem::SoundGroup CreateWeaponSoundVariations(const std::string& weapon_type);
    SoundVariationSystem::SoundGroup CreateCreatureVocalVariations(const std::string& creature_type, int size_category);
    SoundVariationSystem::SoundGroup CreateEnvironmentalVariations(const std::string& environment_type);
    
    // Context builders
    SoundVariationSystem::VariationContext BuildBasicContext(const glm::vec3& world_pos, const glm::vec3& listener_pos);
    SoundVariationSystem::VariationContext BuildMaterialContext(const glm::vec3& world_pos, const glm::vec3& listener_pos, 
                                                               const std::string& material);
    SoundVariationSystem::VariationContext BuildCombatContext(const glm::vec3& world_pos, const glm::vec3& listener_pos,
                                                              float intensity, float player_health);
    
    // Parameter presets
    SoundVariationSystem::VariationParameters GetFootstepParameters();
    SoundVariationSystem::VariationParameters GetWeaponParameters();
    SoundVariationSystem::VariationParameters GetCreatureParameters();
    SoundVariationSystem::VariationParameters GetEnvironmentalParameters();
    SoundVariationSystem::VariationParameters GetUIParameters();
    
    // Learning helpers
    void SetupBasicLearning(SoundVariationSystem& system, const std::string& group_id);
    void ProvideBulkFeedback(SoundVariationSystem& system, const std::vector<std::pair<std::string, float>>& feedback_data);
    
    // Performance optimization helpers
    void OptimizeForLowEndHardware(SoundVariationSystem::SystemSettings& settings);
    void OptimizeForHighEndHardware(SoundVariationSystem::SystemSettings& settings);
}

} // namespace Luminumbra::Client