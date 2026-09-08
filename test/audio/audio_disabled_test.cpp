#include <gtest/gtest.h>

#include "audio/NullAudioManager.h"
#include "core/RuntimeScenarioConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Luminumbra::Client::NullAudioActivation;
using Luminumbra::Client::NullAudioManager;
using Luminumbra::Client::ScenarioHarness::ParseRuntimeScenarioConfig;

auto Parse(std::vector<std::string> arguments, const fs::path& root = {}) {
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return ParseRuntimeScenarioConfig(static_cast<int>(argv.size()), argv.data(), root);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path = fs::temp_directory_path() /
               ("luminumbra-null-audio-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

TEST(AudioDisabledTest, RealLaunchParserRequiresOptInAndDisableWinsInEitherOrder) {
    EXPECT_FALSE(Parse({"client"}).audio_playback_enabled());
    EXPECT_FALSE(Parse({"client", "--no-audio"}).audio_playback_enabled());
    EXPECT_TRUE(Parse({"client", "--enable-audio"}).audio_playback_enabled());
    EXPECT_FALSE(Parse({"client", "--enable-audio", "--no-audio"}).audio_playback_enabled());
    EXPECT_FALSE(Parse({"client", "--no-audio", "--enable-audio"}).audio_playback_enabled());
    EXPECT_TRUE(Parse({"client"}).audio_telemetry_path.empty());
    EXPECT_TRUE(Parse({"client", "--no-audio"}).audio_telemetry_path.empty());
    EXPECT_EQ(Parse({"client", "--audio-telemetry-path", "requested.json"}).audio_telemetry_path,
              fs::path("requested.json"));
}

TEST(AudioDisabledTest, SavedNonzeroVolumesCannotEnablePlaybackOrRetainRequestHistory) {
    TemporaryDirectory directory;
    const auto config = Parse({"client"}, directory.path);
    ASSERT_FALSE(config.audio_playback_enabled());
    NullAudioManager audio(config.audio_telemetry_path);
    ASSERT_TRUE(audio.Init());
    audio.SetMasterVolume(1.0f);
    audio.SetMusicVolume(1.0f);
    audio.SetSfxVolume(1.0f);
    audio.SetBusVolume(Luminumbra::Client::BusId::Ui, 1.0f);
    EXPECT_FALSE(audio.IsPlaybackEnabled());
    const std::string event(128, 'x');
    for (int i = 0; i < 10000; ++i) {
        AudioEventHandle handle = 42;
        ASSERT_TRUE(audio.LoadBank((directory.path / "absent.bank.json").string()));
        ASSERT_TRUE(audio.PlayEvent(event, handle));
        ASSERT_EQ(handle, 0u);
        ASSERT_TRUE(audio.PlayOneShot(event, {}));
        ASSERT_TRUE(audio.PlayOneShot2D(event));
        audio.PlayMusic(event);
        audio.UnloadBank(event);
        audio.SetListenerTransform({}, {}, {});
        audio.Update();
    }
    EXPECT_EQ(audio.RecordedRequestCount(), 0u);
    EXPECT_TRUE(fs::is_empty(directory.path));
    audio.Shutdown();
    EXPECT_FALSE(audio.IsPlaybackEnabled());
    EXPECT_TRUE(fs::is_empty(directory.path));
}

TEST(AudioDisabledTest, ExplicitTelemetryPreservesSuppressionAndReportsItsRealActivation) {
    TemporaryDirectory directory;
    for (const auto activation :
         {NullAudioActivation::ReleaseDefaultOff, NullAudioActivation::ExplicitNoAudio}) {
        const auto path = directory.path / "requested.json";
        NullAudioManager audio(path, activation);
        ASSERT_TRUE(audio.Init());
        const auto missing_bank = directory.path / "missing.bank.json";
        ASSERT_TRUE(audio.LoadBank(missing_bank.string()));
        audio.PlayMusic("music_main_menu");
        audio.SetMasterVolume(1.0f);
        EXPECT_FALSE(audio.IsPlaybackEnabled());
        audio.Shutdown();
        audio.Shutdown();
        std::ifstream stream(path);
        nlohmann::json artifact;
        stream >> artifact;
        EXPECT_EQ(artifact["schema"], "luminumbra.audio.null_telemetry.v1");
        EXPECT_EQ(artifact["activation"]["hardware_backend_initialized"], false);
        EXPECT_EQ(artifact["activation"]["bank_files_touched"], false);
        EXPECT_EQ(artifact["activation"]["flag"],
                  activation == NullAudioActivation::ExplicitNoAudio ? "--no-audio" : "");
        EXPECT_EQ(artifact["activation"]["reason"],
                  activation == NullAudioActivation::ExplicitNoAudio ? "explicit_no_audio"
                                                                     : "release_default_off");
        EXPECT_EQ(artifact["counters"]["shutdown_calls"], 1);
        EXPECT_EQ(artifact["music_requests"], nlohmann::json::array({"music_main_menu"}));
        EXPECT_EQ(audio.RecordedRequestCount(), 2u);
        EXPECT_FALSE(fs::exists(missing_bank));
    }
}

} // namespace
