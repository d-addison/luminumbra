#pragma once

#include "rendering/CaptureManifest.h"

#include <array>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace luminumbra::core {
class SystemConfig;
struct GraphicsValues;
} // namespace luminumbra::core
namespace Luminumbra::Client::ScenarioHarness {
struct RuntimeScenarioConfig;
}

namespace Luminumbra::Rendering {

enum class CaptureFeature {
    AmbientAnimals,
    CreatureSpawning,
    CreatureAI,
    CreatureAnimation,
    Foliage,
    PlantGrowth,
    PlantWind,
    ProceduralPlants,
    Water,
    Rivers,
    Waterfalls,
    Hydrology,
    WeatherHydrology,
    Caves,
    CaveFlourishes,
    Structures,
    Weather,
    WeatherEvents,
    LiveWeatherBridge,
    Precipitation,
    Particles,
    AmbientParticles,
    PrecipitationParticles,
    WaterfallSpray,
    Lightning,
    Clouds,
    CloudShadows,
    Sky,
    Moon,
    Moonlight,
    Stars,
    Count
};
inline constexpr std::size_t kCaptureFeatureCount = static_cast<std::size_t>(CaptureFeature::Count);

enum class CaptureFeatureMode {
    Timelapse,
    FrameScan,
    WorldVisualSweep,
    Scenario,
    TimeOfDaySweep,
    WeatherVisual,
    HeadlessScan,
    HeadlessScene
};
enum class CaptureFeatureChange {
    Unchanged,
    Forced,
    Suppressed,
    Unavailable
};
enum class CaptureFeatureCapability {
    Supported,
    Unsupported,
    Refused
};

// Final producer settings supplied by a mode adapter, never presence evidence.
// The default is deliberately unsupported. Known settings alone cannot certify a
// mode. Reason strings and identities are limited to 256 UTF-8 bytes each.
struct CaptureFeatureAdapter {
    std::optional<bool> enabled;
    CaptureFeatureChange change = CaptureFeatureChange::Unavailable;
    std::string reason = "adapter_unimplemented";
};
struct CaptureFeatureContext {
    CaptureFeatureMode mode = CaptureFeatureMode::Scenario;
    std::array<CaptureFeatureAdapter, kCaptureFeatureCount> adapters{};
    CaptureIdentity asset_hash{"", "resolved_asset_identity_unavailable"};
    CaptureIdentity config_hash{"", "resolved_config_identity_unavailable"};
};
struct CaptureFeatureState {
    bool requested = false;
    std::optional<bool> effective;
    CaptureFeatureChange change = CaptureFeatureChange::Unavailable;
    std::string reason;
    CaptureFeatureCapability capability = CaptureFeatureCapability::Unsupported;
    std::string capability_reason;
};

std::string_view CaptureFeatureName(CaptureFeature feature);

// Immutable setup-time result, bounded by the fixed catalog. Does not mutate
// settings, read files/environment, enable producers or qualify rendered presence.
class CaptureFeatureProfile {
public:
    // Explicit names request a subset when the runtime selection is None.
    // Empty requests, unknown/duplicate names, conflicting selections, invalid
    // adapter metadata/enums and overlong inputs throw before any stream write.
    static CaptureFeatureProfile
    Resolve(const Client::ScenarioHarness::RuntimeScenarioConfig& runtime,
            const luminumbra::core::SystemConfig& systems,
            const luminumbra::core::GraphicsValues& graphics,
            const CaptureFeatureContext& context,
            std::span<const std::string_view> requested = {});

    const std::array<CaptureFeatureState, kCaptureFeatureCount>& features() const;
    CaptureFeatureCapability capability() const;
    std::string Manifest() const;
    // Caller owns the stream/publication lifecycle. A failed write/flush returns
    // false (including exception-enabled streams); partial bytes are not success.
    bool WriteManifest(std::ostream& output) const;

private:
    CaptureFeatureProfile() = default;
    std::array<CaptureFeatureState, kCaptureFeatureCount> m_features{};
    CaptureFeatureContext m_context;
    bool m_all_features = false;
};

// Producer settings only, not evidence that a feature contributes visible pixels.
struct CaptureGameplayFeatures {
    bool ambient_animals = true;
    bool weather = true;
    bool particles = true;
    bool cave_flourishes = true;
    bool ui = true;
    bool operator==(const CaptureGameplayFeatures&) const = default;
};

// A complete supported snapshot may remove capture-only suppressions. Explicit
// determinism fixtures retain their pins; admission never changes UI choices.
class CaptureFeaturePolicy {
public:
    static CaptureFeaturePolicy Resolve(const CaptureFeatureProfile* profile,
                                        bool determinism_fixture = false);
    CaptureGameplayFeatures Apply(const CaptureGameplayFeatures& legacy, bool no_ui) const;
    bool UseLegacyAtmospherePins() const;

private:
    bool m_admitted = false;
};

} // namespace Luminumbra::Rendering
