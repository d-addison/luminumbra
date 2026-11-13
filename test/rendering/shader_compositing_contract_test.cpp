#include <gtest/gtest.h>

#include "luminumbra_client/app/GroundCoverHull.h"
#include "luminumbra_client/rendering/WeatherOverlayModel.h"
#include "luminumbra_client/rendering/WeatherRenderBridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string ReadShader(const std::filesystem::path& relative_path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative_path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void ExpectContains(const std::string& source, const std::string& fragment) {
    EXPECT_NE(source.find(fragment), std::string::npos) << "missing shader contract: " << fragment;
}

std::size_t CountOccurrences(const std::string& source, const std::string& fragment) {
    std::size_t count = 0;
    for (auto pos = source.find(fragment); pos != std::string::npos;
         pos = source.find(fragment, pos + fragment.size())) {
        ++count;
    }
    return count;
}

// Shadow orchestration must delegate only live terrain/glass to ShadowPass.
// Reject owner references and types, class-qualified access and submission/draw calls,
// independent of whitespace. Replay flags and replay-preservation helpers are allowed.
// The retired legacy renderer's compatibility owner is FarLodSystem.
bool HasFarSubmissionAccess(const std::string& source) {
    static const std::regex forbidden(
        R"(\b(m_far_world|m_farlod)\b|)"
        R"(\b(FarWorldRenderer|FarLodSystem)\b|)"
        R"(\b(far_world_renderer|legacy_far_compatibility|submit_ladder|SubmitSelectedFarWorld|DrawFarVolumeGpu)\s*\()");
    return std::regex_search(source, forbidden);
}

// Returns the brace-delimited body that follows the first occurrence of `signature`, or an empty
// string when the signature is absent. The sources this is used on must not contain braces inside
// comments or string literals.
std::string ExtractBlock(const std::string& source, const std::string& signature) {
    const std::size_t signature_pos = source.find(signature);
    if (signature_pos == std::string::npos)
        return {};
    const std::size_t open = source.find('{', signature_pos);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(open, i - open + 1);
        }
    }
    return {};
}

TEST(RenderClockContract, FramePathsReuseTheValidatedCalendar) {
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto header = ReadShader("src/luminumbra_client/rendering/RenderPipeline.h");
    ExpectContains(header, "world::WorldClock m_renderClock;");
    const auto update = ExtractBlock(pipeline, "void RenderPipeline::update_time_of_day(");
    ASSERT_FALSE(update.empty());
    ExpectContains(update, "get_lunar_period_ticks()");
    const auto period = ExtractBlock(header, "std::uint64_t get_lunar_period_ticks() const");
    ExpectContains(period, "return m_renderClock.lunar_period_ticks();");
    EXPECT_EQ(period.find("world::WorldClock"), std::string::npos);
    ExpectContains(update, "SolarTimeFromNoonElapsed(m_elapsedDayPhase, m_renderClock)");
    EXPECT_EQ(update.find("world::WorldClock"), std::string::npos);
    for (const char* signature : {"void RenderPipeline::set_time_of_day(",
                                  "void RenderPipeline::set_time_of_day_tick(",
                                  "void RenderPipeline::set_world_clock("}) {
        const auto body = ExtractBlock(pipeline, signature);
        ASSERT_FALSE(body.empty());
        EXPECT_EQ(body.find("world::WorldClock"), std::string::npos);
        EXPECT_EQ(body.find(", m_renderClock.calendar())"), std::string::npos)
            << "calendar conversion would select a validating overload";
    }
    const auto model = ReadShader("src/luminumbra_client/rendering/TimeOfDayModel.h");
    for (const char* signature :
         {"inline double SolarPhaseFromElapsed(double phase, const world::WorldClock& clock)",
          "inline double ElapsedPhaseFromSolar(double solarPhase, const world::WorldClock& clock)",
          "inline float SolarTimeFromNoonElapsed(double phase, const world::WorldClock& clock)",
          "inline double NoonElapsedFromSolarTime(double phase, const world::WorldClock& clock)"}) {
        const auto body = ExtractBlock(model, signature);
        ASSERT_FALSE(body.empty());
        EXPECT_EQ(body.find("world::WorldClock"), std::string::npos);
        EXPECT_EQ(body.find(", calendar)"), std::string::npos);
    }
    const auto adoption = ExtractBlock(pipeline, "void RenderPipeline::set_render_calendar(");
    ExpectContains(adoption, "const world::WorldClock checked(0, calendar);");
    ExpectContains(adoption, "m_renderClock = checked;");
    ExpectContains(ExtractBlock(pipeline, "void RenderPipeline::set_world_clock("),
                   "m_renderClock = clock;");
}

TEST(ShaderCompositingContract, WaterSamplesTheResolvedSceneAtFarDepth) {
    const std::string source = ReadShader("res/shaders/water.frag");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "bool has_opaque_depth(float depth)");
    ExpectContains(source, "background_has_opaque_depth = has_opaque_depth");
    ExpectContains(source, "refracted_color = texture(u_opaque_scene_color, refraction_uv).rgb");
    ExpectContains(source, "background_has_opaque_depth ? u_sky_color : resolved_background_color");
    ExpectContains(source, "if (!has_opaque_depth(mid_depth))");
    EXPECT_EQ(source.find("refraction_depth"), std::string::npos);
}

TEST(GroundCoverContract, CoarseRocksAndBushesKeepClosedHullsAtEveryDistance) {
    const auto source = ReadShader("src/luminumbra_client/app/ProcgenPalettes.cpp");
    for (const char* kind : {"Rock", "Bush"}) {
        const auto palette =
            ExtractBlock(source, std::string("void BuildProcgen") + kind + "Palette(");
        ASSERT_FALSE(palette.empty());
        for (int lod = 1; lod <= 3; ++lod) {
            ExpectContains(palette,
                           "rp.register_procgen_mesh(base + \".lod" + std::to_string(lod) +
                               "\", BuildGroundCoverMesh(lo, hi, " + (lod == 3 ? "true" : "false") +
                               "));");
        }
        EXPECT_EQ(palette.find("Billboard"), std::string::npos);
    }
    const auto upload = ExtractBlock(source, "BuildGroundCoverMesh(");
    ExpectContains(upload, "BuildGroundCoverHull(lo, hi, farthest)");
    ExpectContains(upload, "f < hull.faceCount");
    ExpectContains(upload, "face.positions[0], face.normal");
    ExpectContains(upload, "face.positions[1], face.normal");
    ExpectContains(upload, "face.positions[2], face.normal");
    ExpectContains(upload, "MeshLoader::CreateFromArrays(vertices, indices)");
}

void ExpectClosedGroundCoverHull(const Luminumbra::Client::App::GroundCoverHull& hull,
                                 const glm::vec3& lo,
                                 const glm::vec3& hi) {
    ASSERT_GT(hull.faceCount, 0u);
    ASSERT_LE(hull.faceCount, hull.faces.size());
    std::vector<glm::vec3> positions;
    std::map<std::pair<std::size_t, std::size_t>, std::pair<int, int>> edges;
    for (std::size_t f = 0; f < hull.faceCount; ++f) {
        std::array<std::size_t, 3> welded{};
        for (std::size_t v = 0; v < welded.size(); ++v) {
            const auto& position = hull.faces[f].positions[v];
            auto found = std::find(positions.begin(), positions.end(), position);
            welded[v] = static_cast<std::size_t>(found - positions.begin());
            if (found == positions.end()) {
                positions.push_back(position);
            }
        }
        for (std::size_t v = 0; v < welded.size(); ++v) {
            const auto a = welded[v];
            const auto b = welded[(v + 1) % welded.size()];
            ASSERT_NE(a, b);
            auto& edge = edges[std::minmax(a, b)];
            ++edge.first;
            edge.second += a < b ? 1 : -1;
        }
    }
    for (const auto& [edge, uses] : edges) {
        SCOPED_TRACE(::testing::Message() << "edge " << edge.first << "-" << edge.second);
        EXPECT_EQ(uses.first, 2);
        EXPECT_EQ(uses.second, 0);
    }
    glm::vec3 center(0.0f);
    glm::vec3 actualLo = positions.front(), actualHi = positions.front();
    for (const auto& position : positions) {
        center += position;
        actualLo = glm::min(actualLo, position);
        actualHi = glm::max(actualHi, position);
    }
    center /= static_cast<float>(positions.size());
    EXPECT_EQ(actualLo, lo);
    EXPECT_EQ(actualHi, hi);
    for (std::size_t f = 0; f < hull.faceCount; ++f) {
        const auto& face = hull.faces[f];
        const glm::vec3 cross = glm::cross(face.positions[1] - face.positions[0],
                                           face.positions[2] - face.positions[0]);
        ASSERT_GT(glm::length(cross), 0.0f);
        EXPECT_GT(glm::dot(cross, face.positions[0] - center), 0.0f);
        EXPECT_NEAR(glm::length(face.normal), 1.0f, 1e-5f);
        EXPECT_GT(glm::dot(glm::normalize(cross), face.normal), 0.99999f);
        EXPECT_TRUE(std::isfinite(face.normal.x));
        EXPECT_TRUE(std::isfinite(face.normal.y));
        EXPECT_TRUE(std::isfinite(face.normal.z));
    }
}

TEST(GroundCoverContract, CoarseHullGeometryIsClosedOutwardAndReducesTheFarthestBudget) {
    using Luminumbra::Client::App::BuildGroundCoverHull;
    // Rock-like tall bounds and bush-like wide bounds, including translated and small props.
    const std::array<std::pair<glm::vec3, glm::vec3>, 4> bounds = {
        {{{-1, -2, -1}, {1, 2, 1}},
         {{-2, -0.2f, -3}, {2, 0.8f, 3}},
         {{3, 1, -5}, {5, 2, -2}},
         {{-0.01f, -0.01f, -0.01f}, {0.01f, 0.01f, 0.01f}}}};
    for (const auto& [lo, hi] : bounds) {
        SCOPED_TRACE(::testing::Message() << "bounds " << lo.x << "," << lo.y << "," << lo.z);
        const auto lod2 = BuildGroundCoverHull(lo, hi, false);
        const auto lod3 = BuildGroundCoverHull(lo, hi, true);
        EXPECT_EQ(lod2.faceCount, 8u);
        EXPECT_LE(lod3.faceCount, 4u);
        EXPECT_LT(lod3.faceCount, lod2.faceCount);
        ExpectClosedGroundCoverHull(lod2, lo, hi);
        ExpectClosedGroundCoverHull(lod3, lo, hi);
    }
}

TEST(GroundCoverContract, DegenerateCoarseHullFacesKeepFiniteFallbackNormals) {
    using Luminumbra::Client::App::BuildGroundCoverHull;
    for (bool farthest : {false, true}) {
        const auto hull = BuildGroundCoverHull(glm::vec3(0), glm::vec3(0), farthest);
        for (std::size_t f = 0; f < hull.faceCount; ++f) {
            EXPECT_EQ(hull.faces[f].normal, glm::vec3(0, 1, 0));
        }
    }
}

TEST(GroundCoverContract, GrassHasSeparatedCurvedBladesAndClutterHasCutoutCorners) {
    const auto source = ReadShader("res/shaders/foliage.frag");
    const auto blade = ExtractBlock(source, "float bladeDistance(");
    ExpectContains(blade, "float t = clamp(p.y / height, 0.0, 1.0);");
    ExpectContains(blade, "float center = root + lean * t * t;");
    ExpectContains(blade, "float halfWidth = 0.11 * (1.0 - t * t);");
    ExpectContains(blade, "return max(abs(p.x - center) - halfWidth, p.y - height);");
    const auto main = ExtractBlock(source, "void main()");
    EXPECT_EQ(CountOccurrences(main, "bladeDistance("), 3u);
    ExpectContains(main, "bladeDistance(p, -0.55, -0.20, 0.78)");
    ExpectContains(main, "bladeDistance(p, 0.0, 0.16, 1.0)");
    ExpectContains(main, "bladeDistance(p, 0.55, 0.20, 0.88)");
    ExpectContains(main, "length(vec2(p.x, p.y * 2.0 - 1.0)) - 1.0");
    ExpectContains(main, "fs_in.color.a > 0.5 ? grassDistance : clutterDistance");
    ExpectContains(main, "max(fwidth(silhouette), 0.015)");
    ExpectContains(main, "smoothstep(-edgeAA, edgeAA, silhouette)) * fs_in.fade");
    ExpectContains(ExtractBlock(main, "if (alpha < 0.02)"), "discard;");
    // CPU, compute and frozen replay draw the same vertex/fragment pair.
    const auto pass = ReadShader("src/luminumbra_client/rendering/passes/FoliagePass.cpp");
    ExpectContains(pass, "res/shaders/foliage.vert");
    ExpectContains(pass, "res/shaders/foliage.frag");
}

TEST(WeatherFrameContract, LiveRainReachesSkyAndOverlayWithoutANearbyStorm) {
    namespace R = Luminumbra::Rendering;
    using Luminumbra::Systems::WeatherCategory;
    Luminumbra::Systems::WeatherSample sample{};
    sample.category = WeatherCategory::Rain;
    sample.wind = {3.0f, 4.0f};
    // Dry, partial and full rain, with and without a stronger nearby storm.
    for (const float precipitation : {-1.0f, 0.0f, 0.4f, 1.0f, 2.0f}) {
        for (const float storm : {-1.0f, 0.0f, 0.8f, 2.0f}) {
            sample.precip_intensity = precipitation;
            sample.storm_intensity = storm;
            const auto state = R::WeatherBridge::BuildWeatherRenderState(sample);
            const auto uniforms = R::ResolveWeatherOverlayState(state, R::WeatherType::None, 0.0f);
            EXPECT_TRUE(uniforms.driven);
            const float rain = std::clamp(precipitation, 0.0f, 1.0f);
            EXPECT_FLOAT_EQ(state.storm_intensity, std::clamp(storm, 0.0f, 1.0f));
            EXPECT_FLOAT_EQ(uniforms.rain_intensity, rain);
            EXPECT_FLOAT_EQ(uniforms.snow_intensity, 0.0f);
            EXPECT_FLOAT_EQ(uniforms.storm_intensity, std::clamp(storm, 0.0f, 1.0f));
            EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(uniforms),
                            std::max(std::clamp(storm, 0.0f, 1.0f), 0.25f * rain));
            EXPECT_FLOAT_EQ(uniforms.wetness, rain);
            EXPECT_FLOAT_EQ(uniforms.fog_density, 0.0f);
            EXPECT_FLOAT_EQ(uniforms.wind_strength, 5.0f / 13.0f);
            EXPECT_EQ(uniforms.wind_direction, glm::vec3(0.6f, 0.0f, 0.8f));
            const auto clouds = R::WeatherBridge::BuildCloudRenderState(sample, 30);
            EXPECT_GE(clouds.coverage_amount, 0.8f);
        }
    }
    // Snow and fog keep their own response; returning to clear removes all rain.
    sample.storm_intensity = 0.0f;
    sample.precip_intensity = 1.0f;
    sample.category = WeatherCategory::Snow;
    const auto snow = R::WeatherBridge::BuildWeatherRenderState(sample);
    EXPECT_FLOAT_EQ(snow.rain_intensity, 0.0f);
    EXPECT_FLOAT_EQ(snow.snow_intensity, 1.0f);
    EXPECT_FLOAT_EQ(snow.storm_intensity, 0.0f);
    sample.precip_intensity = 0.0f;
    for (const auto category : {WeatherCategory::Clear, WeatherCategory::Fog}) {
        sample.category = category;
        const auto state = R::WeatherBridge::BuildWeatherRenderState(sample);
        const auto uniforms = R::ResolveWeatherOverlayState(state, R::WeatherType::Storm, 1.0f);
        EXPECT_FLOAT_EQ(uniforms.rain_intensity, 0.0f);
        EXPECT_FLOAT_EQ(uniforms.storm_intensity, 0.0f);
        EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(uniforms), 0.0f);
        EXPECT_FLOAT_EQ(uniforms.wetness, 0.0f);
        EXPECT_FLOAT_EQ(uniforms.fog_density, category == WeatherCategory::Fog ? 0.4f : 0.0f);
    }
}

TEST(WeatherFrameContract, CalmFullRainKeepsRainIdentityAndQuarterOvercast) {
    namespace R = Luminumbra::Rendering;
    Luminumbra::Systems::WeatherSample sample{};
    sample.category = Luminumbra::Systems::WeatherCategory::Rain;
    sample.precip_intensity = 1.0f;
    const auto state = R::WeatherBridge::BuildWeatherRenderState(sample);
    EXPECT_FLOAT_EQ(state.storm_intensity, 0.0f);
    const auto classification = R::ClassifyWeatherState(state);
    EXPECT_EQ(classification.type, R::WeatherType::Rain);
    EXPECT_FLOAT_EQ(classification.intensity, 1.0f);
    const auto resolved =
        R::ResolveWeatherOverlayState(state, classification.type, classification.intensity);
    const auto direct = R::ResolveWeatherOverlayState({}, R::WeatherType::Rain, 1.0f);
    EXPECT_FLOAT_EQ(resolved.storm_intensity, 0.0f);
    EXPECT_FLOAT_EQ(direct.storm_intensity, 0.0f);
    EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(resolved), 0.25f);
    EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(direct), 0.25f);
}

TEST(WeatherFrameContract, RealStormKeepsStormIdentitySeparateFromRainOvercast) {
    namespace R = Luminumbra::Rendering;
    Luminumbra::Systems::WeatherSample sample{};
    sample.category = Luminumbra::Systems::WeatherCategory::Rain;
    sample.precip_intensity = 1.0f;
    for (const float storm : {0.1f, 0.8f}) {
        sample.storm_intensity = storm;
        const auto state = R::WeatherBridge::BuildWeatherRenderState(sample);
        EXPECT_FLOAT_EQ(state.storm_intensity, storm);
        const auto classification = R::ClassifyWeatherState(state);
        EXPECT_EQ(classification.type, R::WeatherType::Storm);
        EXPECT_FLOAT_EQ(classification.intensity, 1.0f);
        const auto resolved =
            R::ResolveWeatherOverlayState(state, classification.type, classification.intensity);
        EXPECT_FLOAT_EQ(resolved.storm_intensity, storm);
        EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(resolved), std::max(storm, 0.25f));
    }
}

TEST(WeatherFrameContract, ClassificationPreservesThresholdsAndPrecedence) {
    namespace R = Luminumbra::Rendering;
    struct Case {
        float rain;
        float snow;
        float fog;
        float storm;
        R::WeatherType type;
        float intensity;
    };
    const Case cases[]{
        {0.0f, 0.0f, 0.0f, 0.0f, R::WeatherType::None, 0.0f},
        {0.02f, 0.02f, 0.02f, 0.0f, R::WeatherType::None, 0.0f},
        {0.0f, 0.0f, 0.4f, 0.0f, R::WeatherType::Fog, 0.4f},
        {0.021f, 0.0f, 0.4f, 0.0f, R::WeatherType::Rain, 0.021f},
        {1.0f, 0.021f, 0.4f, 0.0f, R::WeatherType::Snow, 0.021f},
        {1.0f, 0.0f, 0.0f, 0.05f, R::WeatherType::Rain, 1.0f},
        {0.05f, 0.0f, 0.0f, 1.0f, R::WeatherType::Rain, 0.05f},
        {0.051f, 0.0f, 0.0f, 0.051f, R::WeatherType::Storm, 0.051f},
        {0.1f, 0.0f, 0.0f, 0.8f, R::WeatherType::Storm, 0.8f},
        {0.1f, 1.0f, 0.0f, 0.8f, R::WeatherType::Storm, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, R::WeatherType::Snow, 1.0f},
        {0.0f, 0.0f, 0.4f, 1.0f, R::WeatherType::Fog, 0.4f},
        {0.0f, 0.0f, 0.0f, 1.0f, R::WeatherType::None, 0.0f},
    };
    for (const auto& test_case : cases) {
        R::WeatherRenderState state;
        state.rain_intensity = test_case.rain;
        state.snow_intensity = test_case.snow;
        state.fog_density = test_case.fog;
        state.storm_intensity = test_case.storm;
        const auto classification = R::ClassifyWeatherState(state);
        EXPECT_EQ(classification.type, test_case.type);
        EXPECT_FLOAT_EQ(classification.intensity, test_case.intensity);
    }
}

TEST(WeatherFrameContract, DirectControlsReplaceStaleDrivenFieldsForBothConsumers) {
    namespace R = Luminumbra::Rendering;
    R::WeatherRenderState stale;
    stale.rain_intensity = stale.storm_intensity = stale.wetness = 1.0f;
    stale.fog_density = 0.8f;
    stale.wind_direction = glm::vec3(0.0f, 0.0f, 1.0f);
    stale.wind_strength = 1.0f;
    for (const float intensity : {0.0f, 0.5f, 1.0f}) {
        for (const auto type : {R::WeatherType::None,
                                R::WeatherType::Rain,
                                R::WeatherType::Snow,
                                R::WeatherType::Fog,
                                R::WeatherType::Storm,
                                static_cast<R::WeatherType>(99)}) {
            const auto uniforms = R::ResolveWeatherOverlayState(stale, type, intensity);
            const bool rain = type == R::WeatherType::Rain || type == R::WeatherType::Storm;
            EXPECT_FLOAT_EQ(uniforms.rain_intensity, rain ? intensity : 0.0f);
            EXPECT_FLOAT_EQ(uniforms.snow_intensity,
                            type == R::WeatherType::Snow ? intensity : 0.0f);
            EXPECT_FLOAT_EQ(uniforms.storm_intensity,
                            type == R::WeatherType::Storm ? intensity : 0.0f);
            EXPECT_FLOAT_EQ(R::ResolveWeatherOvercast(uniforms),
                            type == R::WeatherType::Storm  ? intensity
                            : type == R::WeatherType::Rain ? 0.25f * intensity
                                                           : 0.0f);
            const float fog = type == R::WeatherType::Fog    ? intensity
                              : type == R::WeatherType::Rain ? 0.1f * intensity
                              : type == R::WeatherType::Snow ? 0.05f * intensity
                                                             : 0.0f;
            EXPECT_FLOAT_EQ(uniforms.fog_density, fog);
            EXPECT_FLOAT_EQ(uniforms.wetness, rain ? intensity : 0.0f);
            EXPECT_EQ(uniforms.wind_direction, glm::vec3(1.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(uniforms.wind_strength, 0.3f * intensity);
        }
    }
}

TEST(WeatherFrameContract, FinalScenarioPinsAndConsumersShareTheResolvedFrame) {
    const auto main = ReadShader("src/luminumbra_client/main_client.cpp");
    const auto live = main.find("renderPipeline.set_weather_state(weather_state);");
    const auto pins = main.find("scenario_runner->onPreRenderCapturePins();");
    ASSERT_NE(live, std::string::npos);
    ASSERT_NE(pins, std::string::npos);
    EXPECT_LT(live, pins);
    const auto capture = ReadShader("src/luminumbra_client/core/ScenarioRunnerCapture.cpp");
    const auto pin_body =
        ExtractBlock(capture, "void ScenarioRunnerImpl::onPreRenderCapturePins()");
    const auto drive = ReadShader("src/luminumbra_client/core/ScenarioRunnerDrive.cpp");
    const auto early = ExtractBlock(drive, "ScenarioRunnerImpl::onGameStateInGame(");
    ExpectContains(early, "driveAtmosphereCamera()");
    for (const char* driver :
         {"driveWeatherVisual()", "driveCloudShadow()", "drivePrecipitation()"}) {
        ExpectContains(pin_body, driver);
        EXPECT_EQ(early.find(driver), std::string::npos);
    }
    const auto weather_driver =
        ExtractBlock(drive, "bool ScenarioRunnerImpl::driveWeatherVisual()");
    ExpectContains(weather_driver, "renderPipeline.get_weather_state()");
    ExpectContains(weather_driver, "particles->set_precipitation(");
    ExpectContains(weather_driver, "frame_weather.rain_intensity");
    ExpectContains(weather_driver, "frame_weather.snow_intensity");
    ExpectContains(weather_driver, "particles->set_wind(");
    EXPECT_EQ(CountOccurrences(drive, "wstate.storm_intensity = sample.storm_intensity;"), 2u);
    EXPECT_EQ(drive.find("std::max(sample.storm_intensity,"), std::string::npos);
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto set_weather = ExtractBlock(pipeline, "void RenderPipeline::set_weather_state(");
    ExpectContains(set_weather, "ClassifyWeatherState(m_weather_state)");
    ExpectContains(set_weather, "m_weather_type = classification.type;");
    ExpectContains(set_weather, "m_weather_intensity = classification.intensity;");
    const auto manifest = ReadShader("src/luminumbra_client/rendering/CaptureManifestLive.cpp");
    ExpectContains(manifest, "\"weather_type\", std::int64_t(pipeline.get_weather_type())");
    ExpectContains(manifest, "\"storm_intensity\", double(weather.storm_intensity)");
    const auto context = ExtractBlock(pipeline, "RenderPipeline::make_skybox_context(");
    ExpectContains(context, "ctx.weather_state = &m_weather_state;");
    ExpectContains(context, "ctx.weather_type = m_weather_type;");
    ExpectContains(context, "ctx.weather_intensity = m_weather_intensity;");
    const auto sky = ReadShader("src/luminumbra_client/rendering/passes/SkyboxPass.cpp");
    EXPECT_EQ(CountOccurrences(sky, "ResolveWeatherOverlayState("), 2u);
    ExpectContains(sky, "glm::clamp(ResolveWeatherOvercast(weather), 0.0f, 1.0f)");
    ExpectContains(sky, "setFloat(\"u_stormIntensity\", ResolveWeatherOvercast(weather))");
    ExpectContains(sky, "setFloat(\"u_fogDensity\", weather.fog_density)");
    ExpectContains(sky, "setFloat(\"u_wetness\", weather.wetness)");
}

TEST(FarWorldPositionContract, EveryPositionReadIncludingNeighborsUsesTheSharedHelper) {
    const char* files[]{"lighting_pass.frag",
                        "ssao.frag",
                        "ssao_gtao.frag",
                        "ssao_bilateral_upsample.frag",
                        "scent_decal.frag",
                        "debug_view.frag"};
    const std::size_t expected_calls[]{3, 2, 3, 2, 1, 1};
    for (std::size_t i = 0; i < 6; ++i) {
        SCOPED_TRACE(files[i]);
        const auto source = ReadShader(std::filesystem::path("res/shaders") / files[i]);
        ExpectContains(source, "#include \"far_world_position.glsl\"");
        EXPECT_EQ(CountOccurrences(source, "farWorldPosition("), expected_calls[i]);
        EXPECT_EQ(source.find("texture(gPosition"), std::string::npos);
        EXPECT_EQ(source.find("texelFetch(gPosition"), std::string::npos);
    }
    const auto helper = ReadShader("res/shaders/far_world_position.glsl");
    ExpectContains(helper, "uniform int u_farWorldLadder = 0;");
    ExpectContains(helper, "if (u_farWorldLadder == 0)");
    ExpectContains(helper, "position = texture(storedPosition, uv).rgb;");
    ExpectContains(helper, "depth <= 0.0");
    ExpectContains(helper, "abs(q.w) <= 1e-8");
    ExpectContains(helper, "any(isnan(q)) || any(isinf(q))");
    ExpectContains(helper, "vec4(uv * 2.0 - 1.0, depth, 1.0)");
    ExpectContains(helper, "length(reconstructed) > FAR_WORLD_POSITION_THRESHOLD");
    ExpectContains(helper, "(floor(uv * textureSizePixels) + 0.5) / textureSizePixels");
    EXPECT_EQ(helper.find("depth * 2.0"), std::string::npos);
}

TEST(FarWorldFogContract, FinalFadeFollowsBothCapsAndKeepsUnderwaterSeparate) {
    const auto source = ReadShader("res/shaders/volumetric_lighting.frag");
    const auto compose = ExtractBlock(source, "vec4 composeFinalFog(");
    ExpectContains(compose, "float fog = min(analyticFog, 0.94);");
    ExpectContains(compose, "float outFog = clamp(fog * fx.a + fxFog, 0.0, 0.94);");
    const auto fade = compose.find("float finalTransmittance");
    ASSERT_NE(fade, std::string::npos);
    EXPECT_LT(compose.find("fog = outFog;"), fade);
    EXPECT_EQ(compose.find("0.94", fade), std::string::npos);
    ExpectContains(compose, "(1.0 - fog) * aerialOuterFade(rawRayDistance, 16384.0)");
    ExpectContains(source, "1.0 - smoothstep(0.9 * outerRadius, outerRadius, rayDistance)");
    const auto main = ExtractBlock(source, "void main()");
    ExpectContains(main, "float dist = min(rawRayDistance, u_aerialMaxDistance);");
    ExpectContains(main, "rawRayDistance = length(viewPosition);");
    ExpectContains(main, "clamp(murk, 0.0, 0.92)");
    EXPECT_LT(main.find("if (u_underwater > 0.5)"), main.find("composeFinalFog("));
}

TEST(FarWorldPositionContract, EffectiveFrameProjectionAndPostOverrideDistanceSeams) {
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    for (const char* name : {"gbuffer",
                             "plant",
                             "ssao",
                             "debug_view",
                             "ground_decal",
                             "lighting",
                             "skybox",
                             "water",
                             "aerial"}) {
        const auto body = ExtractBlock(
            pipeline, std::string("RenderContext RenderPipeline::make_") + name + "_context(");
        ExpectContains(body, "m_frame_far_selection.effective == FarWorldMode::Ladder");
        ExpectContains(body, "m_frame_prepared.projection");
        ExpectContains(body, "m_taau_jitter_ndc");
        ExpectContains(body, "ctx.frozen_replay = m_far_world_paired;");
    }
    const auto aerial =
        ExtractBlock(pipeline, "RenderContext RenderPipeline::make_aerial_context(");
    EXPECT_LT(aerial.find("atmo = *s_atmos_override;"), aerial.find("ctx.aerial_max_distance ="));
    ExpectContains(aerial, "World::kFarOuterRadiusMeters");
    const auto context = ReadShader("src/luminumbra_client/rendering/RenderContext.h");
    ExpectContains(context, "selected_projection[2][0] += jitter.x;");
    ExpectContains(context, "selected_projection[2][1] += jitter.y;");
    const char* passes[]{
        "SsaoPass", "AerialPass", "LightingPass", "GroundDecalPass", "DebugViewPass"};
    for (const char* pass : passes) {
        const auto source =
            ReadShader(std::filesystem::path("src/luminumbra_client/rendering/passes") /
                       (std::string(pass) + ".cpp"));
        ExpectContains(source, "ctx.far_world_ladder ? 1 : 0");
        ExpectContains(source, "ctx.gbuffer_inverse_projection");
    }
    for (const char* pass : {"WaterPass", "SkyboxPass"}) {
        const auto source =
            ReadShader(std::filesystem::path("src/luminumbra_client/rendering/passes") /
                       (std::string(pass) + ".cpp"));
        ExpectContains(source, "ctx.far_world_ladder");
        ExpectContains(source,
                       std::string(pass) == "WaterPass" ? "ctx.gbuffer_projection"
                                                        : "ctx.gbuffer_inverse_projection");
    }
    // These existing depth consumers use the zero-to-one convention. Water
    // excludes cleared depth before reconstruction; weather keeps its sky fog.
    const auto water = ReadShader("res/shaders/water.frag");
    const auto weather = ReadShader("res/shaders/weather_system.frag");
    ExpectContains(water, "float z = depth;");
    ExpectContains(weather, "float z = depth;");
}

TEST(FarWorldPositionContract, EveryGbufferDepthWriterConsumesTheSharedLadderProjection) {
    const auto gbuffer = ReadShader("src/luminumbra_client/rendering/passes/GBufferPass.cpp");
    for (const char* name : {"chunks", "static_meshes", "skinned_meshes"}) {
        const auto body =
            ExtractBlock(gbuffer, std::string("void GBufferPass::geometry_pass_") + name + "(");
        ExpectContains(body, "ctx.gbuffer_projection");
        ExpectContains(body, "if (!ctx.far_world_ladder)");
    }
    // Tree impostors inherit the static writer's projection, including jitter.
    ExpectContains(gbuffer, "m_tree_impostor_shader->setMat4(\"u_proj\", static_proj);");
    const auto plant = ReadShader("src/luminumbra_client/rendering/passes/PlantProcgenPass.cpp");
    ExpectContains(plant, "ctx.far_world_ladder");
    ExpectContains(plant, "? ctx.gbuffer_projection");
    ExpectContains(plant, "m_shader->setMat4(\"projection\", projection);");
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto stage = ExtractBlock(pipeline, "void RenderPipeline::execute_stage_gbuffer(");
    ExpectContains(stage, "? gbuffer_ctx.gbuffer_projection");
}

TEST(ShaderCompositingContract, CloudAndAuroraGatesCoverTheFullStormSlab) {
    const std::string source = ReadShader("res/shaders/enhanced_skybox.frag");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "float lowCov = cloudCoverageAt(viewDir.xz * tEnter)");
    ExpectContains(source, "float highCov = cloudCoverageAt(viewDir.xz * tExit)");
    ExpectContains(source, "midCov = max(midCov, max(lowCov, highCov))");
    ExpectContains(source, "float stormGate = 1.0 - smoothstep(0.05, 0.35, u_stormSkyFloor)");
    ExpectContains(source, "overcastGate * stormGate");
}

TEST(ShaderCompositingContract, StarsAreGatedByNightAndComposedBeforeClouds) {
    const std::string source = ReadShader("res/shaders/enhanced_skybox.frag");
    ASSERT_FALSE(source.empty());

    const std::string stars =
        ExtractBlock(source, "vec3 renderStars(vec3 viewDir, float nightIntensity)");
    ExpectContains(stars, "nightIntensity < 0.1");

    const std::string main_body = ExtractBlock(source, "void main()");
    ASSERT_FALSE(main_body.empty());
    const std::size_t stars_pos = main_body.find("skyColor += renderStars(viewDir, nightFactor);");
    const std::size_t clouds_pos =
        main_body.find("skyColor = renderClouds(viewDir, skyColor, dayFactor);");
    ASSERT_NE(stars_pos, std::string::npos) << "renderStars composition missing from main()";
    ASSERT_NE(clouds_pos, std::string::npos) << "renderClouds composition missing from main()";
    EXPECT_LT(stars_pos, clouds_pos);
}

TEST(GlassOitContract, UsesInternalExtentAndSharedLightingDepth) {
    const std::string source =
        ReadShader("src/luminumbra_client/rendering/passes/GlassOitPass.cpp");
    ASSERT_FALSE(source.empty());

    ExpectContains(source, "ctx.internal_w()");
    ExpectContains(source, "ctx.internal_h()");
    ExpectContains(source, "GL_RENDERBUFFER");
    ExpectContains(source, "ctx.lit_scene_depth.id");
    // Screen-size accessors must not leak into the internal-scene path.
    EXPECT_EQ(source.find("screen_width"), std::string::npos);
    EXPECT_EQ(source.find("screen_height"), std::string::npos);
    EXPECT_EQ(source.find("dest_width()"), std::string::npos);
    EXPECT_EQ(source.find("dest_height()"), std::string::npos);
}

TEST(GlassOitContract, DestroyedBeforeLightingDepthInEveryLifecyclePath) {
    const std::string source = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(source.empty());

    const char* const functions[] = {
        "void RenderPipeline::on_resize(u32 new_width, u32 new_height)",
        "void RenderPipeline::cleanup_gpu_resources()",
    };
    for (const char* signature : functions) {
        SCOPED_TRACE(signature);
        const std::string body = ExtractBlock(source, signature);
        ASSERT_FALSE(body.empty());
        const std::size_t destroy_pos = body.find("m_glass_oit_pass->destroy()");
        const std::size_t depth_pos = body.find("m_lighting_pass->destroy_lighting_fbo(");
        ASSERT_NE(destroy_pos, std::string::npos);
        ASSERT_NE(depth_pos, std::string::npos);
        EXPECT_LT(destroy_pos, depth_pos);
    }

    const auto setter = ExtractBlock(source, "void RenderPipeline::set_render_scale(float scale)");
    ExpectContains(setter,
                   "resize_graphics_targets(scale, m_shadow_pass->shadow_map().resolution)");
    const auto apply = ExtractBlock(source, "bool RenderPipeline::apply_graphics(");
    ExpectContains(apply, "resize_graphics_targets(config.render_scale,");
    const auto transaction = ExtractBlock(source, "bool RenderPipeline::resize_graphics_targets(");
    ASSERT_FALSE(transaction.empty());
    const auto destroy_oit = transaction.find("m_glass_oit_pass->destroy()");
    const auto replace = transaction.find("m_render_registry.replace_targets(");
    ASSERT_NE(destroy_oit, std::string::npos);
    ASSERT_NE(replace, std::string::npos);
    EXPECT_LT(destroy_oit, replace); // Before publication AND allocation/failure cleanup.
    EXPECT_EQ(CountOccurrences(transaction, "m_render_registry.replace_targets("), 1u);

    const auto registry = ReadShader("src/luminumbra_client/rendering/RenderResourceRegistry.cpp");
    const auto replacement =
        ExtractBlock(registry, "bool RenderResourceRegistry::replace_targets(");
    const auto cleanup = ExtractBlock(replacement, "class Cleanup");
    ExpectContains(cleanup, "m_registry.destroy_all_owned();");
    const auto guard = replacement.find("const Cleanup cleanup{replacement};");
    const auto allocate = replacement.find("replacement.create_texture(");
    const auto publish = replacement.find("std::swap(value, m_owned_renderbuffers.at(name));");
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(allocate, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(guard, allocate); // Partial allocation and FBO failure retain the cleanup guard.
    EXPECT_LT(allocate, publish);
    ExpectContains(replacement, "!replacement.create_fbo(name, fbo.desc).id");
    // Success retires old targets through the same guard; rollback retires only
    // candidates. In either case every framebuffer precedes depth storage.
    const auto retire = ExtractBlock(registry, "void RenderResourceRegistry::destroy_all_owned()");
    const auto delete_fbos = retire.find("glDeleteFramebuffers(");
    const auto delete_depth = retire.find("glDeleteRenderbuffers(");
    ASSERT_NE(delete_fbos, std::string::npos);
    ASSERT_NE(delete_depth, std::string::npos);
    EXPECT_LT(delete_fbos, delete_depth);
}

TEST(FoliagePreviewContract, BothPreviewPathsKeepTheirLookAndRestoreBeforeOtherViews) {
    const auto preview = ReadShader("src/luminumbra_client/world/WorldgenPreview.cpp");
    ExpectContains(preview, "kFoliageFadeStartM = 60.0f");
    ExpectContains(preview, "kFoliageFadeEndM = 96.0f");
    ExpectContains(preview, "kFoliagePreviewDensityScale = 1.6f");
    for (const char* signature :
         {"bool WorldgenPreview::render(", "bool WorldgenPreview::render_to_backbuffer("}) {
        const auto body = ExtractBlock(preview, signature);
        ExpectContains(body, "rebuild_foliage_if_needed(pipeline);");
    }
    const auto foliage = ExtractBlock(preview, "void WorldgenPreview::rebuild_foliage_if_needed(");
    const auto look = foliage.find("foliage->set_fade_distances(");
    const auto cached = foliage.find("if (m_last_foliage_generation != m_rebuild_generation) {");
    ASSERT_NE(look, std::string::npos);
    ASSERT_NE(cached, std::string::npos);
    EXPECT_LT(look, cached); // Re-entry preserves the preview even with a cached world.
    ExpectContains(foliage, "foliage->set_density_scale(kFoliagePreviewDensityScale);");
    const auto client = ReadShader("src/luminumbra_client/main_client.cpp");
    const auto exit = ExtractBlock(client, "if (worldgenPreview->active() &&");
    ExpectContains(client, "currentState != GameState::MAIN_MENU || !g_uiManager ||");
    ExpectContains(client, "!g_uiManager->GetWorldCreationPreviewState().active");
    ExpectContains(exit, "renderPipeline.apply_graphics(resolved_graphics)");
    ExpectContains(exit, "worldgenPreview->set_active(false)");
    const auto restore = client.find("if (worldgenPreview->active() &&");
    const auto render = client.find("UpdateFoliageScatter(g_app,");
    const auto menu = client.find("RenderMenuScreens(g_app,");
    ASSERT_NE(restore, std::string::npos);
    ASSERT_NE(render, std::string::npos);
    ASSERT_NE(menu, std::string::npos);
    EXPECT_LT(restore, render);
    EXPECT_LT(restore, menu); // Cancellation restores the menu before later gameplay.
}

TEST(FarWorldShadowContract, SignedTagIsLadderOnlyAndAfterAllMaterialPaths) {
    const auto source = ReadShader("res/shaders/g_buffer.frag");
    ASSERT_FALSE(source.empty());
    const auto body = ExtractBlock(source, "void main()");
    ExpectContains(body,
                   "gMetallicAO = vec2(metallic, ao);\n"
                   "#ifdef LUMINUMBRA_FAR_WORLD_LADDER");
    ExpectContains(body,
                   "if (u_farWorldLadder == 1)\n"
                   "        gMetallicAO.g = -(1.0 + ao);\n#endif");
    const auto tag = body.find("gMetallicAO.g = -(1.0 + ao);");
    ASSERT_NE(tag, std::string::npos);
    EXPECT_LT(body.find("case 200u:"), tag);
    EXPECT_EQ(body.find("return;"), std::string::npos);
    EXPECT_EQ(body.find("discard;", tag), std::string::npos);
    // Full byte material ID and the marker lane retain their existing writes.
    ExpectContains(body, "float material_id_normalized = float(fs_in.MaterialID) / 255.0;");
    ExpectContains(
        body, "gNormalMaterial = vec4(encoded_normal * 0.5 + 0.5, 0.0, material_id_normalized);");
    const auto marker = ReadShader("res/shaders/plant_procgen.frag");
    ExpectContains(marker, "gNormalMaterial");
    EXPECT_EQ(marker.find("LUMINUMBRA_FAR_WORLD_LADDER"), std::string::npos);
}

TEST(FarWorldShadowContract, NegativeAOBypassesOnlyTheThreeCascadeLookups) {
    const auto source = ReadShader("res/shaders/lighting_pass.frag");
    const auto body = ExtractBlock(source, "void main()");
    ASSERT_FALSE(body.empty());
    ExpectContains(body, "bool isFarSurface = matData.g < 0.0;");
    ExpectContains(body, "float materialAO = isFarSurface ? -matData.g - 1.0 : matData.g;");
    ExpectContains(body, "if (authored) ao *= clamp(materialAO, 0.0, 1.0);");
    // GLSL conditionals evaluate only the selected operand: far fragments never
    // call the cascade samplers. The untagged operands are the original calls.
    ExpectContains(body,
                   "float shadow = (isFarSurface || u_staticStudio != 0) ? 1.0 : "
                   "CalculateShadow(FragPos, Normal, L_sun, abs(viewPos.z));");
    ExpectContains(body,
                   "vec3 shadowTint = isFarSurface ? vec3(1.0) : "
                   "SampleShadowTint(FragPos, abs(viewPos.z));");
    ExpectContains(body,
                   "float moonShadow = isFarSurface ? 1.0 : "
                   "CalculateShadow(FragPos, Normal, L_moon, abs(viewPos.z));");
    ExpectContains(body, "if (!isFarSurface) moonShadow = mix(moonShadowFloor, 1.0, moonShadow);");
    EXPECT_EQ(CountOccurrences(body, "CalculateShadow("), 2u);
    EXPECT_EQ(CountOccurrences(body, "SampleShadowTint("), 1u);
    ExpectContains(body, "uint MaterialID = uint(round(normalData.a * 255.0));");
    ExpectContains(body, "float markerEmissive = normalData.b;");
    ExpectContains(body, "float cloud_shadow = cloudShadow(FragPos);");
    ExpectContains(body, "* shadow * (1.0 - cloud_shadow) * shadowTint;");
    ExpectContains(body, "texture(u_ssao, TexCoords).r");
    ExpectContains(body, "skyVis = computeSkyVisibility(viewPos, viewNormal);");
}

TEST(CanopyAmbientContract, SkyVisibilityUsesThicknessAndContinuousDistance) {
    const auto source = ReadShader("res/shaders/lighting_pass.frag");
    const auto body = ExtractBlock(source, "float computeSkyVisibility(");
    ASSERT_FALSE(body.empty());
    ExpectContains(body,
                   "u_caveSkyMaxDist <= 0.0 || u_caveThickness <= 0.0 || u_caveSkySteps <= 0");
    ExpectContains(body, "int steps = min(u_caveSkySteps, 32);");
    ExpectContains(body, "float stepLen = u_caveSkyMaxDist / float(steps);");
    ExpectContains(body, "if (s > steps) break;");
    ExpectContains(body, "bool previousValid = true;");
    ExpectContains(body, "float previousDistance = 0.0;");
    ExpectContains(body, "float previousDepthGap = 0.0;");
    ExpectContains(body, "previousValid && previousDepthGap <= 0.05 && depthGap > 0.05");
    ExpectContains(body, "for (int refinement = 0; refinement < 8; ++refinement)");
    ExpectContains(body, "if (refinementTaps >= 8) break;");
    ExpectContains(body, "float midpoint = 0.5 * (frontDistance + backDistance);");
    ExpectContains(body, "previousValid = false;");
    ExpectContains(body, "if (refinedGap <= 0.05)");
    ExpectContains(body, "frontDistance = midpoint;");
    ExpectContains(body, "backDistance = midpoint;");
    ExpectContains(body, "sampPos = refinedPosition;");
    ExpectContains(body, "depthGap = refinedGap;");
    ExpectContains(body, "bool originBracket = s == 1;");
    ExpectContains(body, "if (originBracket)");
    ExpectContains(body, "previousDistance = distanceAlongRay;");
    ExpectContains(body, "bool emptyCrossing = !previousValid && depthGap > 0.05 &&");
    ExpectContains(body, "dot(sampPos - ro, skyDirView) > 0.0;");
    ExpectContains(body, "crossing = crossing || emptyCrossing;");
    ExpectContains(body, "originBracket = originBracket || emptyCrossing;");
    ExpectContains(body, "originBracket = false;");
    const auto refinement = ExtractBlock(body, "if (crossing)");
    EXPECT_EQ(refinement.find("u_caveThickness"), std::string::npos)
        << "intersection selection must not branch or stop at the thickness cutoff";
    ExpectContains(body, "farWorldPosition(gPosition, u_sceneDepth, refinedUv, refinedPosition)");
    ExpectContains(body, "depthGap > 0.05 && depthGap < u_caveThickness");
    ExpectContains(body, "length(sampPos - ro) / u_caveSkyMaxDist");
    ExpectContains(body, "smoothstep(0.5 * u_caveThickness, u_caveThickness, depthGap)");
    ExpectContains(body, "occluded = max(occluded, distanceWeight * thicknessWeight);");
    EXPECT_EQ(body.find("float(s - 1)"), std::string::npos);
    // Legacy and Ladder G-buffers share this march; authored studio bypasses it.
    ExpectContains(body, "farWorldPosition(gPosition, u_sceneDepth, uv, sampPos)");
    ExpectContains(ExtractBlock(source, "void main()"),
                   "skyVis = computeSkyVisibility(viewPos, viewNormal);");
}

namespace {
struct CanopySample {
    double gap;
    double surfaceDistance;
    bool valid;
    bool onScreen;
    double surfaceRise;
};

struct CanopyProbe {
    double visibility;
    int taps;
};

// Scalar mirror of computeSkyVisibility; the source contract above pins the
// origin, crossing predicate, bisection, read budget and both falloff factors.
template<typename Sample>
CanopyProbe ProbeCanopy(const Sample& sample, double maxDistance, double thickness, int steps) {
    if (maxDistance <= 0 || thickness <= 0 || steps <= 0) {
        return {1, 0};
    }
    steps = std::min(steps, 32);
    const double stepLength = maxDistance / steps;
    bool previousValid = true;
    double previousDistance = 0;
    double previousGap = 0;
    int refinementTaps = 0;
    int taps = 0;
    double occluded = 0;
    for (int step = 1; step <= steps; ++step) {
        const double distance = stepLength * step;
        auto hit = sample(distance);
        if (!hit.onScreen) {
            break;
        }
        ++taps;
        if (!hit.valid) {
            previousValid = false;
            previousDistance = distance;
            continue;
        }
        const bool emptyCrossing = !previousValid && hit.gap > 0.05 && hit.surfaceRise > 0;
        const bool crossing =
            (previousValid && previousGap <= 0.05 && hit.gap > 0.05) || emptyCrossing;
        double frontDistance = previousDistance;
        double backDistance = distance;
        previousValid = true;
        previousDistance = distance;
        previousGap = hit.gap;
        if (crossing) {
            bool originBracket = step == 1 || emptyCrossing;
            for (int refinement = 0; refinement < 8; ++refinement) {
                if (refinementTaps >= 8) {
                    break;
                }
                const double midpoint = 0.5 * (frontDistance + backDistance);
                const auto refined = sample(midpoint);
                if (!refined.onScreen) {
                    break;
                }
                ++refinementTaps;
                ++taps;
                if (!refined.valid) {
                    if (originBracket) {
                        frontDistance = midpoint;
                        continue;
                    }
                    break;
                }
                if (refined.gap <= 0.05) {
                    frontDistance = midpoint;
                    originBracket = false;
                } else {
                    backDistance = midpoint;
                    hit = refined;
                }
            }
        }
        if (hit.gap > 0.05 && hit.gap < thickness) {
            const double distanceWeight =
                1 - std::clamp(hit.surfaceDistance / maxDistance, 0.0, 1.0);
            const double ramp =
                std::clamp((hit.gap - 0.5 * thickness) / (0.5 * thickness), 0.0, 1.0);
            const double thicknessWeight = 1 - ramp * ramp * (3 - 2 * ramp);
            occluded = std::max(occluded, distanceWeight * thicknessWeight);
        }
    }
    return {1 - 0.94 * std::clamp(occluded, 0.0, 1.0), taps};
}

// Level perspective camera, fragment (0,-1.7,-12), upward normal, 60 degree
// vertical FOV. A floor supplies the known-open geometry below the horizon.
CanopySample CanopyRoomSample(double distance, double ceilingHeight, double originY = -1.65) {
    const double rayY = originY + distance;
    if (rayY == 0) {
        return {0, 0, false, true, 0};
    }
    const double surfaceY = rayY > 0 ? ceilingHeight : originY - 0.05;
    const double surfaceZ = -12 * surfaceY / rayY;
    return {std::hypot(rayY, 12) - std::hypot(surfaceY, surfaceZ),
            std::hypot(surfaceY - originY, surfaceZ + 12),
            true,
            std::abs(rayY) <= 12 / std::sqrt(3.0),
            surfaceY - originY};
}

void ExpectCanopyMirrorContract() {
    const auto body =
        ExtractBlock(ReadShader("res/shaders/lighting_pass.frag"), "float computeSkyVisibility(");
    ExpectContains(body, "bool previousValid = true;");
    ExpectContains(body, "previousValid && previousDepthGap <= 0.05 && depthGap > 0.05");
    EXPECT_EQ(ExtractBlock(body, "if (crossing)").find("u_caveThickness"), std::string::npos);
    ExpectContains(body, "bool emptyCrossing = !previousValid && depthGap > 0.05 &&");
    ExpectContains(body, "dot(sampPos - ro, skyDirView) > 0.0;");
    ExpectContains(body, "crossing = crossing || emptyCrossing;");
    ExpectContains(body, "originBracket = originBracket || emptyCrossing;");
    const auto empty = ExtractBlock(body, "if (!valid || dot(sampPos, sampPos) < 1e-6)");
    ExpectContains(empty, "previousDistance = distanceAlongRay;");
}
} // namespace

TEST(CanopyAmbientContract, FirstIntervalCeilingIsOccluded) {
    ExpectCanopyMirrorContract();
    for (bool floorVisible : {false, true}) {
        const auto room = [floorVisible](double distance) {
            auto sample = CanopyRoomSample(distance, 1);
            sample.valid = sample.valid && (floorVisible || distance > 1.65);
            return sample;
        };
        ASSERT_GT(room(3).gap, 1.5) << "the first coarse tap has crossed the entire band";
        const auto result = ProbeCanopy(room, 24, 1.5, 8);
        EXPECT_LT(result.visibility, 0.3);
        EXPECT_GT(result.taps, 2);
        EXPECT_LE(result.taps, 40);
    }
    const auto ceilingBeforeOpenGeometry = [](double distance) {
        auto sample = CanopyRoomSample(distance, 0.1, -0.8);
        sample.valid = sample.valid && distance > 0.8;
        return sample;
    };
    EXPECT_LT(ProbeCanopy(ceilingBeforeOpenGeometry, 24, 1.5, 8).visibility, 0.3)
        << "a back-side hit before an empty open-side probe must retain the origin bracket";
}

TEST(CanopyAmbientContract, EmptyCoarseSamplesRetainFiniteCeilingRecovery) {
    ExpectCanopyMirrorContract();
    const auto roof = [](double distance) {
        auto sample = CanopyRoomSample(distance, 2, -3);
        const double rayY = -3 + distance;
        sample.valid = rayY > 0 && rayY >= 24.0 / 60 && rayY <= 24.0 / 2.5;
        return sample;
    };
    ASSERT_FALSE(roof(3).valid);
    ASSERT_NEAR(roof(6).gap, 4.12, 0.01);
    ASSERT_NEAR(roof(9).gap, 8.94, 0.01);
    for (int steps : {8, 16}) {
        const auto result = ProbeCanopy(roof, 24, 1.5, steps);
        EXPECT_LT(result.visibility, 0.4);
        EXPECT_GT(result.taps, 3);
        EXPECT_LE(result.taps, 40);
    }
    // Each recorded list has at most 40 distances, bounded by ProbeCanopy.
    // Recovery must resolve the same crossing even when the final thickness
    // band rejects every observed back-side hit.
    std::vector<double> reference;
    for (double thickness : {0.05, 0.12, 1.5, 8.0}) {
        std::vector<double> distances;
        const auto recordedRoof = [&](double distance) {
            distances.push_back(distance);
            return roof(distance);
        };
        const auto result = ProbeCanopy(recordedRoof, 24, thickness, 8);
        EXPECT_LE(result.taps, 40);
        if (reference.empty()) {
            reference = distances;
        }
        EXPECT_EQ(distances, reference);
        if (thickness <= 0.05) {
            EXPECT_DOUBLE_EQ(result.visibility, 1);
        }
    }
    double cutoff = 1.5;
    for (double distance : reference) {
        const auto hit = roof(distance);
        if (hit.valid && hit.gap > 0.05) {
            cutoff = std::min(cutoff, hit.gap);
        }
    }
    ASSERT_LT(cutoff, 1.5);
    const auto below = ProbeCanopy(roof, 24, cutoff - 0.0001, 8);
    const auto above = ProbeCanopy(roof, 24, cutoff + 0.0001, 8);
    EXPECT_DOUBLE_EQ(below.visibility, 1);
    EXPECT_LT(above.visibility, 1);
    EXPECT_NEAR(below.visibility, above.visibility, 0.0001);
}

TEST(CanopyAmbientContract, EmptyCoarseRecoveryRequiresObservedOverheadBandHit) {
    ExpectCanopyMirrorContract();
    for (double gap : {-1.0, 0.0, 0.05}) {
        const auto open = [gap](double distance) {
            return CanopySample{gap, 1, distance >= 6, true, 5};
        };
        const auto result = ProbeCanopy(open, 6, 1.5, 2);
        EXPECT_DOUBLE_EQ(result.visibility, 1);
        EXPECT_EQ(result.taps, 2) << "an open-side hit cannot start empty-sample recovery";
    }
    for (double rise : {-1.0, 0.0, 0.05, 5.0}) {
        const auto silhouette = [rise](double distance) {
            return CanopySample{8, 1, distance >= 6, true, rise};
        };
        const auto result = ProbeCanopy(silhouette, 6, 1.5, 2);
        EXPECT_DOUBLE_EQ(result.visibility, 1);
        EXPECT_EQ(result.taps, rise > 0 ? 10 : 2);
    }
    for (bool onScreen : {false, true}) {
        const auto interrupted = [onScreen](double distance) {
            const bool present = distance == 6;
            return CanopySample{8, 1, present, present || onScreen || distance == 3, 5};
        };
        const auto result = ProbeCanopy(interrupted, 6, 1.5, 2);
        EXPECT_DOUBLE_EQ(result.visibility, 1);
        EXPECT_EQ(result.taps, onScreen ? 10 : 2);
    }
    const auto openThenHole = [](double distance) {
        return CanopySample{
            distance <= 4.5 ? 0.0 : 8.0, 1, distance != 3 && distance != 5.25, true, 5};
    };
    const auto hole = ProbeCanopy(openThenHole, 6, 1.5, 2);
    EXPECT_DOUBLE_EQ(hole.visibility, 1);
    EXPECT_EQ(hole.taps, 4) << "a refined hole after observed open geometry still aborts";
    const auto observedBand = [](double distance) {
        return CanopySample{0.5, 1, distance == 6, true, 5};
    };
    const auto retained = ProbeCanopy(observedBand, 6, 1.5, 2);
    EXPECT_NEAR(retained.visibility, 0.06 + 0.94 / 6, 1e-12);
    EXPECT_EQ(retained.taps, 10) << "empty refinements retain an observed in-band hit";
    const auto exhausted = [](double distance) {
        return CanopySample{8, 1, distance >= 1.5, true, 5};
    };
    const auto bounded = ProbeCanopy(exhausted, 24, 1.5, 64);
    EXPECT_DOUBLE_EQ(bounded.visibility, 1);
    EXPECT_EQ(bounded.taps, 40) << "empty recovery shares the eight-read refinement budget";
}

TEST(CanopyAmbientContract, CrossingWeightsAreContinuousAtThicknessCutoff) {
    ExpectCanopyMirrorContract();
    // Tune the ceiling so the second coarse gap straddles 1.5 m. The first
    // coarse sample is in front of the ceiling on both sides of the cutoff.
    const double rayY = -1.65 + 6;
    const double rayDepth = std::hypot(rayY, 12);
    double visibility[2]{};
    int index = 0;
    for (double gap : {1.499, 1.501}) {
        const double ceilingHeight = rayY * (1 - gap / rayDepth);
        const auto room = [ceilingHeight](double distance) {
            return CanopyRoomSample(distance, ceilingHeight);
        };
        ASSERT_LE(room(3).gap, 0);
        ASSERT_NEAR(room(6).gap, gap, 1e-12);
        const auto result = ProbeCanopy(room, 24, 1.5, 8);
        EXPECT_LT(result.visibility, 0.4);
        EXPECT_LE(result.taps, 40);
        visibility[index++] = result.visibility;
    }
    EXPECT_NEAR(visibility[0], visibility[1], 0.005);
}

TEST(CanopyAmbientContract, InvalidRefinementAndExhaustedBudgetDoNotInventHits) {
    ExpectCanopyMirrorContract();
    for (double missingDistance : {4.5, 3.75}) {
        for (bool onScreen : {false, true}) {
            const auto missing = [onScreen, missingDistance](double distance) {
                const bool present = distance != missingDistance;
                return CanopySample{distance <= 3 ? 0.0 : 8.0, 1, present, present || onScreen, 0};
            };
            const auto result = ProbeCanopy(missing, 6, 1.5, 2);
            EXPECT_DOUBLE_EQ(result.visibility, 1);
            const int validTaps = missingDistance == 4.5 ? 2 : 3;
            EXPECT_EQ(result.taps, validTaps + (onScreen ? 1 : 0));
        }
    }
    const auto emptyOriginInterval = [](double distance) {
        return CanopySample{8, 1, distance == 3, true, 0};
    };
    const auto empty = ProbeCanopy(emptyOriginInterval, 3, 1.5, 1);
    EXPECT_DOUBLE_EQ(empty.visibility, 1) << "empty probes cannot invent a hit inside the band";
    EXPECT_EQ(empty.taps, 9);
    const auto originHole = [](double distance) {
        return CanopySample{distance <= 1.5 ? 0.0 : 8.0, 1, distance != 2.25, true, 0};
    };
    const auto hole = ProbeCanopy(originHole, 3, 1.5, 1);
    EXPECT_DOUBLE_EQ(hole.visibility, 1)
        << "a hole after observed open geometry invalidates the origin bracket";
    EXPECT_EQ(hole.taps, 3);
    const auto stripes = [](double distance) {
        const auto band = static_cast<int>(std::floor(distance / 0.75 + 0.5));
        return CanopySample{band % 2 == 0 ? 8.0 : -80.0, 1, true, true, 0};
    };
    const auto exhausted = ProbeCanopy(stripes, 24, 1.5, 64);
    EXPECT_DOUBLE_EQ(exhausted.visibility, 1);
    EXPECT_EQ(exhausted.taps, 40);
    const auto laterHit = [](double distance) {
        const double gap = distance < 1.125 || (distance > 1.5 && distance <= 2.25) ? -80
                           : distance > 2.25 && distance <= 3                       ? 0.5
                                                                                    : 8;
        return CanopySample{gap, 1, true, true, 0};
    };
    const auto retained = ProbeCanopy(laterHit, 24, 1.5, 32);
    EXPECT_LT(retained.visibility, 0.2)
        << "an observed in-band hit survives exhaustion of the shared refinement budget";
    EXPECT_EQ(retained.taps, 40);
    const auto interruptedHit = [](double distance) {
        return CanopySample{distance <= 3 ? 0 : 0.5, 1, distance != 4.5, true, 0};
    };
    const auto interrupted = ProbeCanopy(interruptedHit, 6, 1.5, 2);
    EXPECT_NEAR(interrupted.visibility, 0.06 + 0.94 / 6, 1e-12)
        << "missing refinement data retains an already observed in-band sample";
    EXPECT_EQ(interrupted.taps, 3);
    const auto room = [](double distance) {
        return CanopyRoomSample(distance, 1);
    };
    for (double invalid : {0.0, -1.0}) {
        EXPECT_EQ(ProbeCanopy(room, invalid, 1.5, 8).taps, 0);
        EXPECT_EQ(ProbeCanopy(room, 24, invalid, 8).taps, 0);
    }
    for (int invalid : {0, -1}) {
        EXPECT_EQ(ProbeCanopy(room, 24, 1.5, invalid).taps, 0);
    }
}

TEST(CanopyAmbientContract, BiasAndMissingSamplesPreserveThicknessAcceptance) {
    ExpectCanopyMirrorContract();
    const auto shallowFront = [](double distance) {
        return CanopySample{distance <= 3 ? 0.03 : 0.5, 1, true, true, 0};
    };
    const auto crossing = ProbeCanopy(shallowFront, 6, 1.5, 2);
    EXPECT_NEAR(crossing.visibility, 0.06 + 0.94 / 6, 1e-12);
    EXPECT_EQ(crossing.taps, 10) << "a positive gap below the self-bias is still an open bracket";
    for (double gap : {-0.1, 0.01, 0.05, 0.8, 1.4, 1.5, 1.6}) {
        const auto unbracketed = [gap](double distance) {
            return CanopySample{gap, 1, distance != 1.5, true, 0};
        };
        const auto result = ProbeCanopy(unbracketed, 3, 1.5, 2);
        EXPECT_EQ(result.taps, 2) << "missing geometry must invalidate the bracket";
        if (gap > 0.05 && gap < 1.5) {
            EXPECT_LT(result.visibility, 1);
        } else {
            EXPECT_DOUBLE_EQ(result.visibility, 1);
        }
    }
}

TEST(FarWorldShadowContract, SubmissionPredicateAllowsReplayStateAndRejectsDrawAccess) {
    for (const char* source : {
             "shadow_ctx.frozen_replay = m_far_world_paired;",
             "#include \"../FarWorldReplay.h\"",
             "PreserveReplayObject(ReplayObjectKind::LayerFramebuffer, m_shadow_map.fbo_id);",
             "other_m_far_world->Submit(camera, prepare);",
             "m_farlod_state.draw();",
             "other_far_world_renderer().Submit(camera, prepare);",
             "other_legacy_far_compatibility()->draw();",
             "submit_ladder_state();",
             "SubmitSelectedFarWorldReplay();",
             "DrawFarVolumeGpuReplay();",
             "OtherFarWorldRenderer::Submit(camera, prepare);",
             "FarLodSystemState::draw();",
         }) {
        SCOPED_TRACE(source);
        EXPECT_FALSE(HasFarSubmissionAccess(source));
    }
    for (const char* source : {
             "m_far_world->Submit(camera, prepare);",
             "m_far_world -> Submit(camera, prepare);",
             "m_far_world\n\t. Submit(camera, prepare);",
             "m_farlod->draw();",
             "m_farlod \t. draw();",
             "far_world_renderer().Submit(camera, prepare);",
             "far_world_renderer \n().Submit(camera, prepare);",
             "legacy_far_compatibility()->draw();",
             "legacy_far_compatibility \n()->draw();",
             "input.submit_ladder (prepare);",
             "SubmitSelectedFarWorld (input, prepare);",
             "DrawFarVolumeGpu (allocation, driver);",
             "FarWorldRenderer::Submit(camera, prepare);",
             "FarLodSystem \n\t:: draw();",
             "FarLodSystem \t:: draw();",
         }) {
        SCOPED_TRACE(source);
        EXPECT_TRUE(HasFarSubmissionAccess(source));
    }
}

TEST(FarWorldShadowContract, ProductionCascadesHaveOnlyLiveAndGlassInputs) {
    const auto input = ReadShader("src/luminumbra_client/rendering/RenderInputs.h");
    const auto shadow_input = ExtractBlock(input, "struct ShadowPassInput");
    ASSERT_FALSE(shadow_input.empty());
    ExpectContains(shadow_input, "SubmitTerrainChunksFn submit_terrain;");
    EXPECT_EQ(shadow_input.find("FarWorld"), std::string::npos);
    EXPECT_EQ(shadow_input.find("FarLod"), std::string::npos);
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    const auto stage = ExtractBlock(pipeline, "void RenderPipeline::execute_stage_shadow(");
    ASSERT_FALSE(stage.empty());
    ExpectContains(stage, "shadow_input.submit_terrain = make_terrain_submitter();");
    EXPECT_FALSE(HasFarSubmissionAccess(stage));
    // Owner handoffs and class-qualified calls added inside this stage must trip
    // the same source-contract predicate, including dereferences and aliases.
    const auto before_closing_brace = stage.substr(0, stage.size() - 1);
    for (const char* mutation : {
             "m_far_world -> Submit(camera, prepare);",
             "(*m_far_world).Submit(camera, prepare);",
             "auto& far = *m_far_world; far.Submit(camera, prepare);",
             "helper(*m_far_world, camera, prepare);",
             "helper(m_far_world, camera, prepare);",
             "helper(m_far_world.get(), camera, prepare);",
             "m_farlod -> draw();",
             "(*m_farlod).draw();",
             "auto& far = *m_farlod; far.draw();",
             "helper(*m_farlod, camera, prepare);",
             "helper(m_farlod, camera, prepare);",
             "helper(m_farlod.get(), camera, prepare);",
             "legacy_far_compatibility()->draw();",
             "auto* far = legacy_far_compatibility(); far->draw();",
             "helper(legacy_far_compatibility(), camera, prepare);",
             "FarWorldRenderer::Submit(camera, prepare);",
             "FarLodSystem::draw();",
             "DrawFarVolumeGpu(allocation, driver);",
         }) {
        SCOPED_TRACE(mutation);
        EXPECT_TRUE(HasFarSubmissionAccess(before_closing_brace + "\n" + mutation + "\n}"));
    }
    EXPECT_FALSE(HasFarSubmissionAccess("#include \"../FarWorldReplay.h\"\n"
                                        "PreserveReplayObject(kind, id);"));
    EXPECT_FALSE(HasFarSubmissionAccess("shadow_ctx.frozen_replay = m_far_world_paired;"));
    EXPECT_TRUE(HasFarSubmissionAccess("#include \"../FarWorldReplay.h\"\n"
                                       "m_far_world -> Submit(camera, prepare);"));
    EXPECT_TRUE(HasFarSubmissionAccess("shadow_ctx.frozen_replay = m_far_world_paired;\n"
                                       "m_far_world -> Submit(camera, prepare);"));
    EXPECT_TRUE(HasFarSubmissionAccess("FarWorldRenderer* owner;"));
    EXPECT_TRUE(HasFarSubmissionAccess("FarLodSystem* owner;"));
    const auto live =
        ExtractBlock(pipeline, "SubmitTerrainChunksFn RenderPipeline::make_terrain_submitter(");
    ExpectContains(live, "CullHierarchical");
    ExpectContains(live, "draw_chunks_mdi");
    EXPECT_FALSE(HasFarSubmissionAccess(live));
    const auto pass = ReadShader("src/luminumbra_client/rendering/passes/ShadowPass.cpp");
    ExpectContains(pass, "i < ShadowMap::CASCADE_COUNT");
    ExpectContains(pass, "input.submit_terrain(cascade_planes)");
    EXPECT_FALSE(HasFarSubmissionAccess(pass));
}

} // namespace

TEST(FarWorldWaterContract, ProductionWaterUsesIndependentReadinessAndCombinedSubmission) {
    const auto fragment = ReadShader("res/shaders/g_buffer.frag");
    ExpectContains(fragment, "materialId == 200u ? 2 : 1");
    ExpectContains(fragment, "farLiveOwns(local, fs_in.MaterialID)");
    ExpectContains(fragment, "(u_farLiveCells[lo].w & readiness) != 0");
    ExpectContains(fragment, "case 200u: albedo = vec3(0.018, 0.065, 0.11);");
    const auto pipeline = ReadShader("src/luminumbra_client/rendering/RenderPipeline.cpp");
    ExpectContains(pipeline, "upload.water_snapshot_version = chunk.water_mesh_version;");
    // Drawing and ownership must certify the retained presentation, even while
    // the current snapshot is waiting for adoption. Check calls within their
    // owning functions without depending on local names or line wrapping.
    const auto draw = ExtractBlock(pipeline, "void RenderPipeline::execute_stage_water(");
    EXPECT_TRUE(std::regex_search(draw, std::regex(R"(\.\s*PresentedLadderWaterEligible\s*\()")));
    const auto prepare = ExtractBlock(pipeline, "void RenderPipeline::prepare_frame(");
    EXPECT_TRUE(
        std::regex_search(prepare, std::regex(R"(\.\s*SetPresentedLadderWaterUpload\s*\()")));
    const auto header = ReadShader("src/luminumbra_client/rendering/RenderPipeline.h");
    const auto eligibility = ExtractBlock(header, "bool PresentedLadderWaterEligible(");
    EXPECT_TRUE(std::regex_search(
        eligibility,
        std::regex(
            R"(return\s+LadderWaterPassEligible\s*\(\s*mesh_version\s*,\s*mesh_version\s*,)"
            R"(\s*element_count\s*,\s*LadderUploadValid\s*\(\s*\)\s*,\s*enabled\s*\)\s*;)")));
    const auto ownership = ExtractBlock(header, "void SetPresentedLadderWaterUpload(");
    EXPECT_TRUE(std::regex_search(
        ownership,
        std::regex(
            R"(\bSetLadderWaterUpload\s*\(\s*upload\s*,\s*mesh_version\s*,\s*enabled\s*\))")));
    const auto pass = ReadShader("src/luminumbra_client/rendering/passes/GBufferPass.cpp");
    ExpectContains(pass, "stats.far_water_indices += far_stats.water_indices;");
}
