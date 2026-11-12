#include "AudioPropagationSystem.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
#include <string_view>
#endif

namespace Luminumbra::Client {

#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
// Supplied only by the regression executable; shipping builds have no hooks.
void AudioPropagationTestEvent(std::string_view event, size_t worker_count);
std::chrono::steady_clock::time_point AudioPropagationTestNow();
unsigned int AudioPropagationTestWorkerCount();
#endif

namespace {
std::chrono::steady_clock::time_point CacheNow() {
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
    return AudioPropagationTestNow();
#else
    return std::chrono::steady_clock::now();
#endif
}
} // namespace

AudioPropagationSystem::AudioPropagationSystem(Luminumbra::Systems::PhysicsSystem* physicsSystem)
    : m_physicsSystem(physicsSystem) {
    // Initialize worker threads for async computation
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
    const unsigned int num_threads = AudioPropagationTestWorkerCount();
#else
    const unsigned int num_threads = std::max(1U, std::thread::hardware_concurrency() / 2U);
#endif
    // Allocate ownership slots before starting any worker.
    m_worker_threads.reserve(num_threads);
    try {
        for (unsigned int i = 0; i < num_threads; ++i) {
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
            m_worker_threads.emplace_back([this] {
                WorkerThreadFunction();
                AudioPropagationTestEvent("worker exited", 0);
            });
            AudioPropagationTestEvent("worker started", m_worker_threads.size());
#else
            m_worker_threads.emplace_back(&AudioPropagationSystem::WorkerThreadFunction, this);
#endif
        }

#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
        AudioPropagationTestEvent("before logging", m_worker_threads.size());
#endif
        LUMINUMBRA_CORE_INFO("Audio Propagation System initialized with {} worker threads",
                             num_threads);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            m_shutdown = true;
        }
        m_task_cv.notify_all();
        for (auto& thread : m_worker_threads) {
            if (thread.joinable()) {
                thread.join();
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
                AudioPropagationTestEvent("worker joined", 0);
#endif
            }
        }
        throw;
    }
}

AudioPropagationSystem::~AudioPropagationSystem() {
    {
        // Publishing shutdown under the task mutex keeps a worker that has just evaluated its wait
        // predicate from missing the notification and blocking destruction in join().
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_shutdown = true;
    }
    m_task_cv.notify_all();

    for (auto& thread : m_worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

AudioPropagationResult AudioPropagationSystem::CalculatePropagation(const glm::vec3& source,
                                                                    const glm::vec3& listener,
                                                                    float max_distance,
                                                                    int max_reflections) {
    auto start_time = std::chrono::high_resolution_clock::now();

    AudioPropagationResult result;

    PropagationSettings settings;
    PropagationCache::Key key;
    {
        LUMIN_PROFILE_LOCK(
            std::lock_guard<std::mutex>, lock, m_cache.cache_mutex, "Audio cache lock wait");
        m_cache.CleanupOldEntries(CacheNow());
        settings = m_settings;
        key = PropagationCache::MakeKey(
            source, listener, max_distance, max_reflections, m_settings_generation);
        auto it = m_cache.entries.find(key);
        if (it != m_cache.entries.end()) {
            UpdatePerformanceMetrics(0.1f, true); // Cache hit
            return it->second.result;
        }
    }

    // Calculate direct path
    result.direct_path = CalculateDirectPath(source, listener, settings);

    // Calculate reflection paths if within range
    float distance = glm::distance(source, listener);
    if (distance <= max_distance && max_reflections > 0) {
        result.reflection_paths = CalculateReflectionPaths(
            source, listener, std::min(max_reflections, settings.max_reflection_depth), settings);
    }

    // Calculate overall occlusion
    if (m_physicsSystem) {
        result.total_occlusion = m_physicsSystem->calculate_audio_occlusion(source, listener);
    }

    // Environmental analysis for reverb
    EnvironmentalAnalysis env = AnalyzeEnvironment(listener, std::min(distance, 20.0f));
    result.reverb_time = env.rt60_estimate;

    // Calculate early reflections delay
    if (!result.reflection_paths.empty()) {
        result.early_reflections_delay = result.reflection_paths[0].delay_ms;

        // Find dominant reflection direction
        glm::vec3 weighted_direction(0.0f);
        float total_weight = 0.0f;

        for (const auto& path : result.reflection_paths) {
            if (path.points.size() >= 2) {
                glm::vec3 direction = glm::normalize(path.points[1] - path.points[0]);
                float weight = path.volume_multiplier;
                weighted_direction += direction * weight;
                total_weight += weight;
            }
        }

        if (total_weight > 0.0f) {
            result.dominant_reflection_direction = weighted_direction / total_weight;
        }
    }

#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
    AudioPropagationTestEvent("before cache publication", 0);
#endif
    // An in-flight calculation owns its settings snapshot but cannot repopulate a newer cache.
    {
        LUMIN_PROFILE_LOCK(
            std::lock_guard<std::mutex>, lock, m_cache.cache_mutex, "Audio cache lock wait");
        const auto now = CacheNow();
        m_cache.CleanupOldEntries(now);
        if (key.settings_generation == m_settings_generation) {
            m_cache.Store(key, result, now);
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    float calculation_time =
        std::chrono::duration<float, std::milli>(end_time - start_time).count();
    UpdatePerformanceMetrics(calculation_time, false);

    return result;
}

void AudioPropagationSystem::CalculatePropagationAsync(
    const glm::vec3& source,
    const glm::vec3& listener,
    std::function<void(AudioPropagationResult)> callback,
    float max_distance) {
    {
        LUMIN_PROFILE_LOCK(std::lock_guard<std::mutex>, lock, m_task_mutex, "Audio task lock wait");
        m_pending_tasks.push_back({source, listener, max_distance, std::move(callback)});
    }
    m_task_cv.notify_one();
}

AudioPropagationSystem::ThunderCue
AudioPropagationSystem::ComputeThunderCue(const glm::vec3& strike_position,
                                          const glm::vec3& listener) const {
    // thin thunder cue . Geometric flash-to-bang delay only --
    // distance / speed_of_sound -- plus a smooth distance attenuation. No reflection
    // tracing, no environment analysis, no physics dependency: this is the minimal
    // additive hook, not new propagation machinery. speed_of_sound is the existing
    // setting (343 m/s default).
    ThunderCue cue;
    cue.distance = glm::distance(strike_position, listener);
    const auto settings = GetSettings();
    const float c = settings.speed_of_sound > 1.0f ? settings.speed_of_sound : 343.0f;
    cue.delay_seconds = cue.distance / c;
    // Inverse-ish falloff: distant strikes are quieter (and the low rumble carries),
    // bounded to [0, 1]. ~600 m halves the volume.
    cue.volume_multiplier = 1.0f / (1.0f + cue.distance / 600.0f);
    return cue;
}

AudioPropagationSystem::AmbienceBed AudioPropagationSystem::ComputeAmbienceBed(
    const glm::vec3& listener, float requested_volume, float analysis_radius) {
    //  (AU1): occlude the weather ambience bed by the listener's enclosure.
    // Outdoor weather (wind/rain) is a non-positional bed; an enclosed space muffles
    // it. We reuse the existing environmental analysis (RT60 + enclosure estimate)
    // rather than tracing new paths -- this is the additive ambience hook, not new
    // propagation machinery. Pure read of the environment; no sim writes.
    AmbienceBed bed;
    bed.requested_volume = std::clamp(requested_volume, 0.0f, 1.0f);

    EnvironmentalAnalysis env = AnalyzeEnvironment(listener, analysis_radius);
    // Openness: an enclosed space (cave/room) muffles the outdoor bed to a floor;
    // open sky passes it through. avg_absorption is high in soft enclosed spaces.
    float openness =
        env.is_enclosed ? std::clamp(0.35f - env.avg_absorption * 0.25f, 0.05f, 0.35f) : 1.0f;
    bed.openness = openness;
    bed.volume = bed.requested_volume * openness;
    return bed;
}

AudioPropagationSystem::WaterfallRoar
AudioPropagationSystem::ComputeWaterfallRoar(const glm::vec3& crest_position,
                                             float drop_height,
                                             const glm::vec3& listener,
                                             float max_distance) {
    // positional waterfall roar. Distance attenuation reuses the
    // same inverse-falloff shape as ComputeThunderCue (additive hook, not new
    // propagation). The loop volume also scales with the fall's drop height: a
    // tall fall roars louder than a trickle. The low-pass term rises with
    // distance because the bright spray hiss attenuates faster than the low
    // rumble. Pure read; no sim writes.
    WaterfallRoar roar;
    roar.distance = glm::distance(crest_position, listener);
    const float md = max_distance > 1.0f ? max_distance : 400.0f;
    if (roar.distance >= md) {
        roar.volume = 0.0f;
        roar.low_pass = 1.0f;
        roar.audible = false;
        return roar;
    }
    // Drop-height loudness: ramps a 3 m fall up to a full roar by ~24 m of drop.
    const float drop_gain = std::clamp(drop_height / 24.0f, 0.15f, 1.0f);
    // ~120 m halves the volume (a fall carries less far than thunder).
    const float dist_atten = 1.0f / (1.0f + roar.distance / 120.0f);
    roar.volume = std::clamp(drop_gain * dist_atten, 0.0f, 1.0f);
    // Muffle (low-pass) rises smoothly with normalized distance.
    roar.low_pass = std::clamp(roar.distance / md, 0.0f, 1.0f);
    roar.audible = roar.volume > 0.02f;
    return roar;
}

AudioPropagationPath AudioPropagationSystem::CalculateDirectPath(
    const glm::vec3& source, const glm::vec3& listener, const PropagationSettings& settings) {
    AudioPropagationPath path;
    path.is_direct_path = true;
    path.points = {source, listener};
    path.total_distance = glm::distance(source, listener);
    path.delay_ms = (path.total_distance / settings.speed_of_sound) * 1000.0f;

    if (m_physicsSystem) {
        auto ray_result = m_physicsSystem->audio_raycast(source, listener);
        if (ray_result.hit) {
            path.absorption_factor = ray_result.material_absorption;
            path.volume_multiplier = 1.0f - path.absorption_factor;
        } else {
            path.absorption_factor =
                AudioPropagationUtils::CalculateAirAbsorption(path.total_distance, 1000.0f);
            path.volume_multiplier = 1.0f - path.absorption_factor;
        }
    } else {
        // Fallback: simple distance attenuation
        path.volume_multiplier = 1.0f / (1.0f + path.total_distance * 0.1f);
    }

    return path;
}

std::vector<AudioPropagationPath>
AudioPropagationSystem::CalculateReflectionPaths(const glm::vec3& source,
                                                 const glm::vec3& listener,
                                                 int max_bounces,
                                                 const PropagationSettings& settings) {
    std::vector<AudioPropagationPath> paths;

    if (!m_physicsSystem)
        return paths;

    // Generate reflection points from physics system
    std::vector<glm::vec3> reflection_points =
        m_physicsSystem->calculate_audio_reflection_points(source, listener, max_bounces);

    for (size_t i = 0; i < reflection_points.size(); ++i) {
        AudioPropagationPath path;
        path.is_direct_path = false;
        path.bounces = static_cast<int>(i) + 1;

        // Build path through reflection points
        path.points.push_back(source);
        for (size_t j = 0; j <= i; ++j) {
            path.points.push_back(reflection_points[j]);
        }
        path.points.push_back(listener);

        // Calculate total distance and absorption
        path.total_distance = 0.0f;
        path.absorption_factor = 0.0f;

        for (size_t j = 1; j < path.points.size(); ++j) {
            float segment_distance = glm::distance(path.points[j - 1], path.points[j]);
            path.total_distance += segment_distance;

            // Add material absorption for each reflection
            if (j > 1 && j < path.points.size() - 1) { // Reflection points
                path.absorption_factor += 0.2f;        // Base reflection loss
            }
        }

        // Air absorption
        path.absorption_factor +=
            AudioPropagationUtils::CalculateAirAbsorption(path.total_distance, 1000.0f);
        path.volume_multiplier = std::max(0.01f, 1.0f - path.absorption_factor);
        path.delay_ms = (path.total_distance / settings.speed_of_sound) * 1000.0f;

        // Skip paths that are too quiet or too late
        if (path.volume_multiplier > settings.min_reflection_energy && path.delay_ms < 500.0f) {
            paths.push_back(path);
        }
    }

    // Sort by delay time (early reflections first)
    std::sort(paths.begin(),
              paths.end(),
              [](const AudioPropagationPath& a, const AudioPropagationPath& b) {
                  return a.delay_ms < b.delay_ms;
              });

    return paths;
}

AudioPropagationSystem::EnvironmentalAnalysis
AudioPropagationSystem::AnalyzeEnvironment(const glm::vec3& position, float analysis_radius) {
    EnvironmentalAnalysis analysis;
    analysis.room_center = position;

    if (!m_physicsSystem) {
        // Fallback: assume outdoor environment
        analysis.room_volume = analysis_radius * analysis_radius * analysis_radius * 8.0f;
        analysis.surface_area = analysis_radius * analysis_radius * 6.0f * 4.0f;
        analysis.avg_absorption = 0.1f; // Outdoor
        analysis.is_enclosed = false;
        analysis.rt60_estimate = 0.2f;
        return analysis;
    }

    // Sample points around the listener to determine environment
    std::vector<glm::vec3> sample_directions = {{1.0f, 0.0f, 0.0f},
                                                {-1.0f, 0.0f, 0.0f}, // X axis
                                                {0.0f, 1.0f, 0.0f},
                                                {0.0f, -1.0f, 0.0f}, // Y axis
                                                {0.0f, 0.0f, 1.0f},
                                                {0.0f, 0.0f, -1.0f}, // Z axis
                                                // Additional diagonal samples
                                                {0.707f, 0.707f, 0.0f},
                                                {-0.707f, 0.707f, 0.0f},
                                                {0.707f, 0.0f, 0.707f},
                                                {0.0f, 0.707f, 0.707f}};

    float total_distance = 0.0f;
    int enclosed_rays = 0;
    float total_absorption = 0.0f;

    for (const auto& direction : sample_directions) {
        glm::vec3 target = position + direction * analysis_radius;
        auto ray_result = m_physicsSystem->audio_raycast(position, target);

        if (ray_result.hit) {
            enclosed_rays++;
            total_distance += ray_result.distance;
            total_absorption += ray_result.material_absorption;
        } else {
            total_distance += analysis_radius;
        }
    }

    analysis.is_enclosed =
        (static_cast<float>(enclosed_rays) > static_cast<float>(sample_directions.size()) * 0.5f);

    if (analysis.is_enclosed) {
        float avg_wall_distance = total_distance / static_cast<float>(sample_directions.size());
        analysis.room_volume = avg_wall_distance * avg_wall_distance * avg_wall_distance * 2.0f;
        analysis.surface_area = avg_wall_distance * avg_wall_distance * 6.0f;
        analysis.avg_absorption = total_absorption / static_cast<float>(std::max(1, enclosed_rays));
        analysis.rt60_estimate =
            CalculateRT60(analysis.room_volume, analysis.surface_area, analysis.avg_absorption);
    } else {
        // Outdoor environment
        analysis.room_volume = analysis_radius * analysis_radius * analysis_radius * 8.0f;
        analysis.surface_area = analysis_radius * analysis_radius * 6.0f * 4.0f;
        analysis.avg_absorption = 0.1f;
        analysis.rt60_estimate = 0.2f;
    }

    return analysis;
}

float AudioPropagationSystem::CalculateRT60(float room_volume,
                                            float surface_area,
                                            float avg_absorption) {
    // Sabine formula: RT60 = 0.161 * V / (A * Sa)
    // Where V = volume, A = absorption coefficient, Sa = surface area
    float absorption_area = avg_absorption * surface_area;
    if (absorption_area < 0.01f)
        absorption_area = 0.01f; // Avoid division by zero

    return 0.161f * room_volume / absorption_area;
}

AudioPropagationSystem::PropagationCache::Key
AudioPropagationSystem::PropagationCache::MakeKey(const glm::vec3& source,
                                                  const glm::vec3& listener,
                                                  float max_distance,
                                                  int max_reflections,
                                                  uint64_t generation) {
    // Exact bit identity avoids quantization overflow and hash collisions reusing other inputs.
    return {.inputs = {std::bit_cast<uint32_t>(source.x),
                       std::bit_cast<uint32_t>(source.y),
                       std::bit_cast<uint32_t>(source.z),
                       std::bit_cast<uint32_t>(listener.x),
                       std::bit_cast<uint32_t>(listener.y),
                       std::bit_cast<uint32_t>(listener.z),
                       std::bit_cast<uint32_t>(max_distance),
                       static_cast<uint32_t>(max_reflections)},
            .settings_generation = generation};
}

size_t AudioPropagationSystem::PropagationCache::KeyHash::operator()(const Key& key) const {
    // FNV-1a over eight 32-bit inputs followed by the 64-bit generation, low byte first.
    uint64_t hash = 14695981039346656037ULL;
    const auto fold = [&hash](uint64_t value, unsigned int bytes) {
        for (unsigned int byte = 0; byte < bytes; ++byte) {
            hash = (hash ^ (value & 0xffU)) * 1099511628211ULL;
            value >>= 8;
        }
    };
    for (const uint32_t input : key.inputs) {
        fold(input, 4);
    }
    fold(key.settings_generation, 8);
    return static_cast<size_t>(hash);
}

void AudioPropagationSystem::PropagationCache::CleanupOldEntries(
    std::chrono::steady_clock::time_point now) {
    // Insertion order is also expiry order. Each of at most max_entries nodes is retired once.
    while (!insertion_order.empty()) {
        const auto oldest = entries.find(insertion_order.front());
        if (std::chrono::duration<float>(now - oldest->second.timestamp).count() <
            max_age_seconds) {
            break;
        }
        entries.erase(oldest);
        insertion_order.pop_front();
    }
}

void AudioPropagationSystem::PropagationCache::Store(const Key& key,
                                                     const AudioPropagationResult& result,
                                                     std::chrono::steady_clock::time_point now) {
    // Concurrent duplicate calculations retain the first publication and its expiry.
    if (entries.contains(key)) {
        return;
    }
    insertion_order.push_back(key);
    try {
#ifdef LUMINUMBRA_AUDIO_PROPAGATION_TESTING
        AudioPropagationTestEvent("before cache insertion", 0);
#endif
        entries.emplace(key, CacheEntry{.result = result, .timestamp = now});
    } catch (...) {
        insertion_order.pop_back();
        throw;
    }
    if (entries.size() > max_entries) {
        entries.erase(insertion_order.front());
        insertion_order.pop_front();
    }
}

void AudioPropagationSystem::UpdatePerformanceMetrics(float calculation_time_ms, bool cache_hit) {
    std::lock_guard<std::mutex> lock(m_metrics_mutex);
    if (cache_hit) {
        m_metrics.cache_hits++;
    } else {
        m_metrics.cache_misses++;
        m_metrics.completed_calculations++;
        m_metrics.last_calculation_time_ms = calculation_time_ms;

        // Update running average
        float alpha = 0.1f; // Smoothing factor
        m_metrics.avg_calculation_time_ms =
            m_metrics.avg_calculation_time_ms * (1.0f - alpha) + calculation_time_ms * alpha;
    }
}

void AudioPropagationSystem::WorkerThreadFunction() {
    LUMIN_PROFILE_THREAD("Audio propagation worker");
    while (!m_shutdown) {
        AsyncTask task;

        // Wait for task
        {
            LUMIN_PROFILE_LOCK(
                std::unique_lock<std::mutex>, lock, m_task_mutex, "Audio task lock wait");
            LUMIN_PROFILE_WAIT("Audio condition wait", m_task_cv.wait(lock, [this] {
                return !m_pending_tasks.empty() || m_shutdown;
            }));

            if (m_shutdown)
                break;

            if (!m_pending_tasks.empty()) {
                task = std::move(m_pending_tasks.back());
                m_pending_tasks.pop_back();
            } else {
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_metrics_mutex);
            m_metrics.active_calculations++;
        }

        LUMIN_PROFILE_ZONE_N("Audio worker job");
        // Process task
        AudioPropagationResult result =
            CalculatePropagation(task.source, task.listener, task.max_distance);

        // Execute callback
        if (task.callback) {
            task.callback(result);
        }

        {
            std::lock_guard<std::mutex> lock(m_metrics_mutex);
            m_metrics.active_calculations--;
        }
    }
}

// === UTILITY FUNCTIONS ===

namespace AudioPropagationUtils {

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float LinearToDb(float linear) {
    return 20.0f * std::log10(std::max(linear, 0.0001f));
}

float CalculateAirAbsorption(float distance, float frequency_hz, float humidity) {
    // ISO 9613-1 standard for atmospheric absorption

    // Simplified air absorption coefficient
    float absorption_coefficient = 0.0f;

    if (frequency_hz < 1000.0f) {
        absorption_coefficient = 0.0001f * frequency_hz / 1000.0f;
    } else if (frequency_hz < 4000.0f) {
        absorption_coefficient = 0.0002f + 0.0003f * (frequency_hz - 1000.0f) / 3000.0f;
    } else {
        absorption_coefficient = 0.0005f + 0.002f * (frequency_hz - 4000.0f) / 16000.0f;
    }

    // Apply humidity factor
    float humidity_factor = 1.0f + (humidity - 50.0f) / 100.0f * 0.2f;
    absorption_coefficient *= humidity_factor;

    return std::min(0.9f, absorption_coefficient * distance / 100.0f);
}

float FrequencyToWavelength(float frequency_hz, float speed_of_sound) {
    return speed_of_sound / frequency_hz;
}

bool IsFrequencyAudible(float frequency_hz) {
    return frequency_hz >= 20.0f && frequency_hz <= 20000.0f;
}

float CalculateLoudness(float amplitude, float frequency_hz) {
    // A-weighting approximation for loudness perception
    float a_weight = 1.0f;

    if (frequency_hz < 1000.0f) {
        a_weight = frequency_hz / 1000.0f;
    } else if (frequency_hz > 4000.0f) {
        a_weight = std::max(0.1f, 4000.0f / frequency_hz);
    }

    return amplitude * a_weight;
}

} // namespace AudioPropagationUtils

} // namespace Luminumbra::Client
