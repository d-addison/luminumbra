#include "rendering/CaptureFeatureProfile.h"

#include "core/IsolationConfig.h"
#include "core/RuntimeScenarioConfig.h"

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
using Client::ScenarioHarness::IsolationLayer::All;
using Feature = CaptureFeature;
using Change = CaptureFeatureChange;
using Capability = CaptureFeatureCapability;
using Selection = Client::ScenarioHarness::RuntimeScenarioConfig::FeatureProfile;

constexpr std::array<std::string_view, kCaptureFeatureCount> kNames{"ambient_animals",
                                                                    "creature_spawning",
                                                                    "creature_ai",
                                                                    "creature_animation",
                                                                    "foliage",
                                                                    "plant_growth",
                                                                    "plant_wind",
                                                                    "procedural_plants",
                                                                    "water",
                                                                    "rivers",
                                                                    "waterfalls",
                                                                    "hydrology",
                                                                    "weather_hydrology",
                                                                    "caves",
                                                                    "cave_flourishes",
                                                                    "structures",
                                                                    "weather",
                                                                    "weather_events",
                                                                    "live_weather_bridge",
                                                                    "precipitation",
                                                                    "particles",
                                                                    "ambient_particles",
                                                                    "precipitation_particles",
                                                                    "waterfall_spray",
                                                                    "lightning",
                                                                    "clouds",
                                                                    "cloud_shadows",
                                                                    "sky",
                                                                    "moon",
                                                                    "moonlight",
                                                                    "stars"};
constexpr std::array<std::string_view, 8> kModes{"timelapse",
                                                 "frame_scan",
                                                 "world_visual_sweep",
                                                 "scenario",
                                                 "timeofday_sweep",
                                                 "weather_visual",
                                                 "headless_scan",
                                                 "headless_scene"};

void ValidateText(const std::string& text, bool allow_empty) {
    if (text.size() > 256 || (!allow_empty && text.empty())) {
        throw std::invalid_argument("invalid capture feature metadata length");
    }
    // JSON's strict UTF-8 encoder validates before any output is offered.
    try {
        (void)nlohmann::json(text).dump();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("invalid capture feature metadata UTF-8");
    }
}

void ValidateIdentity(const CaptureIdentity& identity) {
    ValidateText(identity.value, true);
    ValidateText(identity.unavailable, true);
    if (identity.value.empty() == identity.unavailable.empty()) {
        throw std::invalid_argument("capture identity requires a value or an unavailable reason");
    }
}

void ValidateAdapter(const CaptureFeatureAdapter& adapter, bool requested) {
    ValidateText(adapter.reason, false);
    bool valid = false;
    switch (adapter.change) {
        case Change::Unavailable:
            valid = !adapter.enabled.has_value();
            break;
        case Change::Unchanged:
            valid = adapter.enabled.has_value() && adapter.enabled == requested;
            break;
        case Change::Forced:
            valid = adapter.enabled == true;
            break;
        case Change::Suppressed:
            valid = adapter.enabled == false;
            break;
    }
    if (!valid) {
        throw std::invalid_argument("inconsistent capture feature adapter state");
    }
}

std::uint32_t RenderLayer(Feature feature) {
    namespace Layer = Client::ScenarioHarness::IsolationLayer;
    switch (feature) {
        case Feature::Water:
        case Feature::Rivers:
            return Layer::Water;
        case Feature::Foliage:
            return Layer::Foliage;
        case Feature::Particles:
        case Feature::AmbientParticles:
        case Feature::PrecipitationParticles:
        case Feature::WaterfallSpray:
            return Layer::Particles;
        case Feature::Lightning:
            return Layer::Lightning;
        default:
            return All;
    }
}

std::string_view DisabledSetting(Feature feature,
                                 const luminumbra::core::SystemConfig& systems,
                                 const luminumbra::core::GraphicsValues& graphics) {
    using Key = luminumbra::core::SysKey;
    // Only actual gates belong here. Creature-spawn, plant-growth, ecology,
    // tree-wind and moonlight registry flags select tuning/compiled defaults;
    // disabling those flags does not establish a disabled producer.
    if (feature == Feature::WeatherHydrology && !systems.enabled(Key::SimHydrologyWeather)) {
        return "sim.hydrology_weather_disabled";
    }
    if (feature == Feature::WeatherEvents && !systems.enabled(Key::SimWeatherEvents)) {
        return "sim.weather_events_disabled";
    }
    if (feature == Feature::LiveWeatherBridge && !systems.enabled(Key::RenderLiveWeather)) {
        return "render.live_weather_disabled";
    }
    if (feature == Feature::Foliage && !graphics.grass_enabled) {
        return "graphics.grass_disabled";
    }
    return {};
}

void Suppress(CaptureFeatureState& state, std::string_view reason) {
    state.effective = false;
    state.change = Change::Suppressed;
    state.reason = reason;
}

std::string_view CapabilityName(Capability capability) {
    switch (capability) {
        case Capability::Supported:
            return "supported";
        case Capability::Unsupported:
            return "unsupported";
        case Capability::Refused:
            return "refused";
    }
    return "unsupported";
}

std::string_view ChangeName(Change change) {
    switch (change) {
        case Change::Unchanged:
            return "unchanged";
        case Change::Forced:
            return "forced";
        case Change::Suppressed:
            return "suppressed";
        case Change::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

nlohmann::json IdentityJson(const CaptureIdentity& identity) {
    return {{"value",
             identity.value.empty() ? nlohmann::json(nullptr) : nlohmann::json(identity.value)},
            {"unavailable", identity.unavailable}};
}
} // namespace

std::string_view CaptureFeatureName(CaptureFeature feature) {
    const auto index = static_cast<std::size_t>(feature);
    if (index >= kNames.size()) {
        throw std::invalid_argument("unknown capture feature");
    }
    return kNames[index];
}

CaptureFeatureProfile
CaptureFeatureProfile::Resolve(const Client::ScenarioHarness::RuntimeScenarioConfig& runtime,
                               const luminumbra::core::SystemConfig& systems,
                               const luminumbra::core::GraphicsValues& graphics,
                               const CaptureFeatureContext& context,
                               std::span<const std::string_view> requested) {
    // All work/storage is bounded by the fixed catalog and 256 bytes per metadata
    // field. Isolation parsing is bounded at 1024 bytes before it allocates.
    if (static_cast<std::size_t>(context.mode) >= kModes.size() ||
        runtime.isolation_layers.size() > 1024 || runtime.isolation_backdrop.size() > 256 ||
        requested.size() > kCaptureFeatureCount ||
        (runtime.capture_feature_profile != Selection::None &&
         runtime.capture_feature_profile != Selection::AllFeatures)) {
        throw std::invalid_argument("invalid capture feature profile input");
    }
    const bool all_features = runtime.capture_feature_profile == Selection::AllFeatures;
    if ((all_features && !requested.empty()) || (!all_features && requested.empty())) {
        throw std::invalid_argument("capture feature profile requires one nonempty selection");
    }
    ValidateIdentity(context.asset_hash);
    ValidateIdentity(context.config_hash);
    std::array<bool, kCaptureFeatureCount> selected{};
    selected.fill(all_features);
    for (const auto name : requested) {
        if (name.size() > 64) {
            throw std::invalid_argument("capture feature name too long");
        }
        const auto found = std::find(kNames.begin(), kNames.end(), name);
        if (found == kNames.end()) {
            throw std::invalid_argument("unknown capture feature");
        }
        const auto index = static_cast<std::size_t>(found - kNames.begin());
        if (selected[index]) {
            throw std::invalid_argument("duplicate capture feature");
        }
        selected[index] = true;
    }
    for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
        ValidateAdapter(context.adapters[index], selected[index]);
    }

    const auto isolation = Client::ScenarioHarness::ParseIsolationConfig(
        runtime.isolation_layers, runtime.isolation_backdrop);
    CaptureFeatureProfile result;
    result.m_context = context;
    result.m_all_features = all_features;
    for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
        const auto feature = static_cast<Feature>(index);
        const auto& adapter = context.adapters[index];
        auto& state = result.m_features[index];
        state.requested = selected[index];
        state.effective = adapter.enabled;
        state.change = adapter.change;
        state.reason = adapter.reason;
        // Explicit final observations describe transient mode policy after pins.
        // Baseline controls fill gaps; they cannot overwrite a reported override.
        if (adapter.change != Change::Forced && adapter.change != Change::Suppressed) {
            const auto disabled = DisabledSetting(feature, systems, graphics);
            if (!disabled.empty()) {
                Suppress(state, disabled);
            }
            const auto layer = RenderLayer(feature);
            if (layer != All && !isolation.renders(layer)) {
                Suppress(state, "isolation_layer_suppressed");
            }
            if ((feature == Feature::Sky || feature == Feature::Moon || feature == Feature::Stars ||
                 feature == Feature::Clouds) &&
                isolation.backdrop != Client::ScenarioHarness::BackdropMode::Scene) {
                Suppress(state, "isolation_backdrop_replaces_sky");
            }
            if (feature == Feature::AmbientAnimals && runtime.active()) {
                Suppress(state, "scenario_suppresses_ambient_animals");
            }
        }
        // A known requested disablement is refused even if its adapter is
        // missing. Other unobserved settings remain unsupported.
        state.capability_reason = adapter.reason;
        if (state.requested && state.effective == false) {
            state.capability = Capability::Refused;
            state.capability_reason = state.reason;
        } else if (adapter.enabled.has_value()) {
            state.capability = Capability::Supported;
        }
    }
    return result;
}

const std::array<CaptureFeatureState, kCaptureFeatureCount>&
CaptureFeatureProfile::features() const {
    return m_features;
}

CaptureFeatureCapability CaptureFeatureProfile::capability() const {
    auto aggregate = m_context.asset_hash.value.empty() || m_context.config_hash.value.empty()
                         ? Capability::Unsupported
                         : Capability::Supported;
    for (const auto& state : m_features) {
        if (!state.requested) {
            continue;
        }
        if (state.capability == Capability::Refused) {
            return Capability::Refused;
        }
        if (state.capability == Capability::Unsupported) {
            aggregate = Capability::Unsupported;
        }
    }
    return aggregate;
}

std::string CaptureFeatureProfile::Manifest() const {
    // At most 31 rows, two 256-byte reasons per row and two identities. Even
    // worst-case JSON escaping fits within 128 KiB; no world-sized storage.
    auto entries = nlohmann::json::array();
    for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
        const auto& state = m_features[index];
        entries.push_back({{"feature", kNames[index]},
                           {"requested", state.requested},
                           {"effective",
                            state.effective.has_value() ? nlohmann::json(*state.effective)
                                                        : nlohmann::json(nullptr)},
                           {"change", ChangeName(state.change)},
                           {"reason", state.reason},
                           {"capability", CapabilityName(state.capability)},
                           {"capability_reason", state.capability_reason}});
    }
    const nlohmann::json manifest{
        {"schema", "luminumbra.capture_capabilities.v1"},
        {"profile", m_all_features ? "all_features" : "explicit"},
        {"mode", kModes[static_cast<std::size_t>(m_context.mode)]},
        {"capability", CapabilityName(capability())},
        {"capability_reason",
         capability() == Capability::Refused       ? "requested_feature_suppressed"
         : capability() == Capability::Unsupported ? "adapter_or_identity_unavailable"
                                                   : "requested_settings_supported"},
        {"asset_hash", IdentityJson(m_context.asset_hash)},
        {"config_hash", IdentityJson(m_context.config_hash)},
        {"presence", "not_evaluated"},
        {"features", std::move(entries)}};
    return manifest.dump(2) + '\n';
}

bool CaptureFeatureProfile::WriteManifest(std::ostream& output) const {
    const auto text = Manifest();
    try {
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        return output.good();
    } catch (...) {
        // A streambuf may rethrow its own exception when badbit exceptions
        // are enabled. All failures of this caller-owned stream mean false.
        return false;
    }
}

} // namespace Luminumbra::Rendering
