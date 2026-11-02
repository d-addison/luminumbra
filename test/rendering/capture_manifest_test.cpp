#include "../support/UniqueTempPath.h"
#include "luminumbra_common/persistence/SavedInputLimits.h"
#include "rendering/CaptureManifest.h"
#include "rendering/FarWorldReplay.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace Luminumbra::Rendering;

CaptureTime FixedClock() {
    return CaptureTime(std::chrono::milliseconds(1234));
}

CaptureManifestInput Input() {
    CaptureManifestInput input;
    input.build_id = {"0.3.0+01234567", ""};
    input.asset_hash = {"0123456789abcdef", ""};
    input.shader_hash = {"", "compiled_sources_not_retained"};
    input.world_hash = {"", "authoritative_hash_unavailable"};
    input.asset_hash_scope = "preset_content_identity";
    input.world_seed = "seed\"with\\escapes";
    input.simulation_tick = std::numeric_limits<std::uint64_t>::max();
    input.width = 800;
    input.height = 600;
    for (std::size_t index = 0; index < input.view.size(); ++index) {
        input.view[index] = std::nextafter(static_cast<float>(index), 20.0f);
        input.projection[index] = -input.view[index];
    }
    input.position = {1.0f, 2.0f, 3.0f};
    input.forward = {0.0f, 0.0f, -1.0f};
    input.render_settings = {
        {"z_enabled", true}, {"a_scale", 0.67}, {"quality_preset", std::string("Ultra")}};
    return input;
}

std::string Serialize(const CaptureManifestInput& input) {
    return BuildCaptureManifest(
        input, FixedClock(), FixedClock(), CaptureManifestStatus::Complete, "capture_finished");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class CaptureManifestTest : public testing::Test {
protected:
    void SetUp() override {
        directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("capture-manifest-");
        capture = directory / "scene.ppm";
        manifest = CaptureManifestPath(capture);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path directory;
    std::filesystem::path capture;
    std::filesystem::path manifest;
};

TEST_F(CaptureManifestTest, FixedClockIsByteIdenticalAndKeysAreLexicographic) {
    const auto input = Input();
    const auto first = Serialize(input);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, Serialize(input));
    const auto parsed = nlohmann::json::parse(first);
    EXPECT_EQ(parsed.at("schema"), 1);
    EXPECT_EQ(parsed.at("started_utc"), "1970-01-01T00:00:01.234Z");
    EXPECT_EQ(parsed.at("finished_utc"), parsed.at("started_utc"));
    EXPECT_EQ(parsed.at("simulation_tick").get<std::uint64_t>(), input.simulation_tick);
    EXPECT_EQ(parsed.at("world_seed"), input.world_seed);
    std::size_t previous = 0;
    for (const auto* key : {"asset_hash",
                            "asset_hash_scope",
                            "build_id",
                            "camera",
                            "finished_utc",
                            "reason",
                            "render_settings",
                            "resolution",
                            "schema",
                            "shader_hash",
                            "shader_hash_unavailable",
                            "simulation_tick",
                            "started_utc",
                            "status",
                            "world_hash",
                            "world_hash_unavailable",
                            "world_seed"}) {
        const auto position = first.find(std::string("\n  \"") + key + "\":");
        ASSERT_NE(position, std::string::npos);
        EXPECT_GT(position, previous);
        previous = position;
    }
    EXPECT_LT(first.find("\"a_scale\":"), first.find("\"z_enabled\":"));
    ASSERT_TRUE(WithCaptureManifest(capture, input, [] { return true; }, FixedClock));
    EXPECT_EQ(Read(manifest), first);
    const auto second_capture = directory / "other.ppm";
    ASSERT_TRUE(WithCaptureManifest(second_capture, input, [] { return true; }, FixedClock));
    EXPECT_EQ(Read(CaptureManifestPath(second_capture)), first);
}

TEST_F(CaptureManifestTest, MatricesRoundTripFloat32AndStateIsUnchanged) {
    const auto input = Input();
    const auto parsed = nlohmann::json::parse(Serialize(input));
    const auto& camera = parsed.at("camera");
    EXPECT_EQ(camera.at("matrix_order"), "column_major");
    EXPECT_EQ(camera.at("position"), input.position);
    EXPECT_EQ(camera.at("forward"), input.forward);
    for (std::size_t index = 0; index < input.view.size(); ++index) {
        EXPECT_EQ(camera.at("view").at(index).get<float>(), input.view[index]);
        EXPECT_EQ(camera.at("projection").at(index).get<float>(), input.projection[index]);
    }
    EXPECT_EQ(Serialize(input), Serialize(Input()));
}

TEST_F(CaptureManifestTest, PreparedCameraSurvivesLivePoseChangesAndRefusesInvalidSnapshots) {
    auto input = Input();
    FarReplayCamera prepared;
    prepared.view = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, -5, -4, 3, 1};
    prepared.projection = input.projection;
    prepared.zoom = 70;
    prepared.near_plane = 0.25f;
    prepared.far_plane = 10000;
    prepared.width = 800;
    prepared.height = 600;
    ASSERT_TRUE(SetCaptureManifestCamera(input, prepared));
    EXPECT_EQ(input.view, prepared.view);
    EXPECT_EQ(input.projection, prepared.projection);
    EXPECT_EQ(input.position, (std::array<float, 3>{3, 4, 5}));
    EXPECT_EQ(input.forward, (std::array<float, 3>{1, 0, 0}));
    EXPECT_EQ(input.vertical_fov_degrees, 70);
    EXPECT_EQ(input.near_plane, 0.25f);
    EXPECT_EQ(input.far_plane, 10000);
    const auto original = Serialize(input);
    for (int variant = 0; variant < 9; ++variant) {
        auto invalid = prepared;
        switch (variant) {
            case 0:
                invalid.width = 799;
                break;
            case 1:
                invalid.height = 599;
                break;
            case 2:
                invalid.view[0] = std::numeric_limits<float>::quiet_NaN();
                break;
            case 3:
                invalid.projection[0] = std::numeric_limits<float>::infinity();
                break;
            case 4:
                invalid.zoom = 180;
                break;
            case 5:
                invalid.near_plane = 0;
                break;
            case 6:
                invalid.far_plane = invalid.near_plane;
                break;
            case 7:
                invalid.view[0] = std::numeric_limits<float>::max();
                invalid.view[12] = std::numeric_limits<float>::max();
                break;
            case 8:
                invalid.zoom = std::numeric_limits<float>::quiet_NaN();
                break;
        }
        EXPECT_FALSE(SetCaptureManifestCamera(input, invalid)) << variant;
        EXPECT_EQ(Serialize(input), original) << variant;
    }
    auto no_extent = input;
    no_extent.width = 0;
    EXPECT_FALSE(SetCaptureManifestCamera(no_extent, prepared));
}

TEST_F(CaptureManifestTest, UnavailableIdentitiesRequireReasonsAndNeverInventHashes) {
    auto input = Input();
    input.build_id = {"", "engine_identity_unavailable"};
    input.asset_hash = {"", "asset_identity_unavailable"};
    const auto parsed = nlohmann::json::parse(Serialize(input));
    for (const auto* key : {"build_id", "asset_hash", "shader_hash", "world_hash"}) {
        EXPECT_TRUE(parsed.at(key).is_null());
        EXPECT_FALSE(parsed.at(std::string(key) + "_unavailable").get<std::string>().empty());
    }
    input.shader_hash = {"actual_retained_identity", "ignored_when_known"};
    const auto known = nlohmann::json::parse(Serialize(input));
    EXPECT_EQ(known.at("shader_hash"), "actual_retained_identity");
    EXPECT_FALSE(known.contains("shader_hash_unavailable"));
    input.world_hash = {"", ""};
    EXPECT_TRUE(Serialize(input).empty());
}

TEST_F(CaptureManifestTest, IncompletePrecedesPayloadAndFinalVerdictFollowsIt) {
    ASSERT_TRUE(WithCaptureManifest(
        capture,
        Input(),
        [&] {
            const auto pending = nlohmann::json::parse(Read(manifest));
            EXPECT_EQ(pending.at("status"), "incomplete");
            EXPECT_TRUE(pending.at("finished_utc").is_null());
            EXPECT_FALSE(std::filesystem::exists(capture));
            std::ofstream payload(capture, std::ios::binary);
            payload << "payload";
            payload.close();
            return static_cast<bool>(payload);
        },
        FixedClock));
    EXPECT_EQ(Read(capture), "payload");
    EXPECT_EQ(nlohmann::json::parse(Read(manifest)).at("status"), "complete");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(manifest).concat(".tmp")));
    // Replacing an already-complete manifest exercises Windows replacement too.
    ASSERT_TRUE(WithCaptureManifest(capture, Input(), [] { return true; }, FixedClock));
    EXPECT_EQ(nlohmann::json::parse(Read(manifest)).at("status"), "complete");
}

TEST_F(CaptureManifestTest, PayloadFailureAndAbandonmentCannotPublishComplete) {
    EXPECT_FALSE(WithCaptureManifest(capture, Input(), [] { return false; }, FixedClock));
    const auto failed = nlohmann::json::parse(Read(manifest));
    EXPECT_EQ(failed.at("status"), "failed");
    EXPECT_EQ(failed.at("reason"), "capture_failed");
    EXPECT_FALSE(failed.at("finished_utc").is_null());
    EXPECT_THROW(WithCaptureManifest(
                     capture,
                     Input(),
                     []() -> bool { throw std::runtime_error("capture stopped"); },
                     FixedClock),
                 std::runtime_error);
    EXPECT_EQ(nlohmann::json::parse(Read(manifest)).at("status"), "incomplete");
}

TEST_F(CaptureManifestTest, StaleTemporaryAndInvalidParentRefuseWithoutCallingPayload) {
    const auto temporary = std::filesystem::path(manifest).concat(".tmp");
    {
        std::ofstream stream(temporary);
        stream << "partial";
    }
    bool called = false;
    EXPECT_FALSE(WithCaptureManifest(
        capture,
        Input(),
        [&] {
            called = true;
            return true;
        },
        FixedClock));
    EXPECT_FALSE(called);
    EXPECT_EQ(Read(temporary), "partial");
    EXPECT_FALSE(std::filesystem::exists(manifest));
    const auto blocked = directory / "file";
    {
        std::ofstream stream(blocked);
        stream << "untouched";
    }
    EXPECT_FALSE(WithCaptureManifest(
        blocked / "scene.ppm",
        Input(),
        [&] {
            called = true;
            return true;
        },
        FixedClock));
    EXPECT_FALSE(called);
    EXPECT_EQ(Read(blocked), "untouched");
}

TEST_F(CaptureManifestTest, FinalPublicationFailureRetainsIncompleteEvidence) {
    CaptureManifest writer(capture, Input(), FixedClock);
    EXPECT_FALSE(writer.Finish(true));
    ASSERT_TRUE(writer.Begin());
    EXPECT_FALSE(writer.Begin());
    const auto temporary = std::filesystem::path(manifest).concat(".tmp");
    {
        std::ofstream stream(temporary);
        stream << "occupied";
    }
    EXPECT_FALSE(writer.Finish(true));
    EXPECT_EQ(nlohmann::json::parse(Read(manifest)).at("status"), "incomplete");
    EXPECT_EQ(Read(temporary), "occupied");
    EXPECT_FALSE(writer.Finish(true));
    CaptureManifest no_clock(directory / "no-clock.ppm", Input(), nullptr);
    EXPECT_FALSE(no_clock.Begin());
}

TEST_F(CaptureManifestTest, RenameFailureNeverDeletesTheDestinationOrTemporaryEvidence) {
    ASSERT_TRUE(std::filesystem::create_directory(manifest));
    {
        std::ofstream stream(manifest / "sentinel");
        stream << "untouched";
    }
    CaptureManifest writer(capture, Input(), FixedClock);
    EXPECT_FALSE(writer.Begin());
    EXPECT_EQ(Read(manifest / "sentinel"), "untouched");
    EXPECT_EQ(
        nlohmann::json::parse(Read(std::filesystem::path(manifest).concat(".tmp"))).at("status"),
        "incomplete");
}

TEST_F(CaptureManifestTest, InvalidNumbersExtentsAndMetadataAreRefusedBeforeWriting) {
    for (int variant = 0; variant < 18; ++variant) {
        auto input = Input();
        switch (variant) {
            case 0:
                input.width = 0;
                break;
            case 1:
                input.height = -1;
                break;
            case 2:
                input.width = std::numeric_limits<int>::max();
                break;
            case 3:
                input.view[15] = std::numeric_limits<float>::infinity();
                break;
            case 4:
                input.projection[0] = std::numeric_limits<float>::quiet_NaN();
                break;
            case 5:
                input.render_settings["bad"] = std::numeric_limits<double>::infinity();
                break;
            case 6:
                input.near_plane = input.far_plane;
                break;
            case 7:
                input.vertical_fov_degrees = 180.0f;
                break;
            case 8:
                input.world_seed.assign(
                    Luminumbra::Persistence::SavedInputLimits::kWorldSeedBytes + 1, 'x');
                break;
            case 9:
                input.render_settings[std::string(65, 'x')] = true;
                break;
            case 10:
                input.render_settings[""] = true;
                break;
            case 11:
                input.render_settings["text"] = std::string(257, 'x');
                break;
            case 12:
                input.world_seed = std::string(1, static_cast<char>(0xff));
                break;
            case 13:
                input.position[0] = std::numeric_limits<float>::quiet_NaN();
                break;
            case 14:
                input.forward[0] = std::numeric_limits<float>::infinity();
                break;
            case 15:
                input.near_plane = 0;
                break;
            case 16:
                input.build_id.unavailable.assign(257, 'x');
                break;
            case 17:
                input.asset_hash.value.assign(257, 'x');
                break;
        }
        EXPECT_TRUE(Serialize(input).empty()) << variant;
        EXPECT_FALSE(WithCaptureManifest(
            capture,
            input,
            [] {
                ADD_FAILURE();
                return true;
            },
            FixedClock));
        EXPECT_FALSE(std::filesystem::exists(manifest));
    }
    auto input = Input();
    input.world_seed.assign(Luminumbra::Persistence::SavedInputLimits::kWorldSeedBytes, 'x');
    const auto maximum_seed = nlohmann::json::parse(Serialize(input));
    EXPECT_EQ(maximum_seed.at("world_seed"), input.world_seed);
    for (int index = 0; index < 129; ++index)
        input.render_settings[std::to_string(index)] = true;
    EXPECT_TRUE(Serialize(input).empty());
    EXPECT_TRUE(BuildCaptureManifest(
                    Input(), FixedClock(), FixedClock(), CaptureManifestStatus::Complete, "")
                    .empty());
    EXPECT_TRUE(
        BuildCaptureManifest(
            Input(), FixedClock(), FixedClock(), static_cast<CaptureManifestStatus>(99), "invalid")
            .empty());
}

TEST_F(CaptureManifestTest, SidecarNamingUsesTheCaptureStemForEveryFormat) {
    EXPECT_TRUE(CaptureManifestPath({}).empty());
    EXPECT_TRUE(CaptureManifestPath(directory / "").empty());
    CaptureManifest no_capture({}, Input(), FixedClock);
    EXPECT_FALSE(no_capture.Begin());
    EXPECT_EQ(CaptureManifestPath(directory / "scene.ppm"), directory / "scene.manifest.json");
    EXPECT_EQ(CaptureManifestPath(directory / "scene.png"), directory / "scene.manifest.json");
    EXPECT_EQ(CaptureManifestPath(directory / "scene.json"), directory / "scene.manifest.json");
    EXPECT_EQ(CaptureManifestPath(directory / "scene.with.dots.ppm"),
              directory / "scene.with.dots.manifest.json");
}
} // namespace
