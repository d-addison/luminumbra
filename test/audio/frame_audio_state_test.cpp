#include "app/FrameAudioState.h"

#include <entt/entt.hpp>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace {
using Luminumbra::Client::BusId;
using Luminumbra::Client::App::FrameAudioState;

class RecordingAudio final : public Luminumbra::Client::IAudioManager {
public:
    std::vector<std::string> stopped;
    bool Init() override {
        return true;
    }
    void Update() override {}
    void Shutdown() override {}
    bool IsPlaybackEnabled() const override {
        return true;
    }
    bool LoadBank(const std::string&) override {
        return true;
    }
    void UnloadBank(const std::string&) override {}
    void SetListenerTransform(const glm::vec3&, const glm::vec3&, const glm::vec3&) override {}
    bool PlayEvent(const AudioEventID&, AudioEventHandle&) override {
        return true;
    }
    bool PlayOneShot(const AudioEventID&, const glm::vec3&, BusId) override {
        return true;
    }
    bool PlayOneShot2D(const AudioEventID&, BusId, float) override {
        return true;
    }
    void PlayMusic(const AudioEventID&) override {}
    void StopMusic() override {}
    void PlayAmbientLoop(const AudioEventID&, const glm::vec3&, float) override {}
    void StopAmbientLoop(const AudioEventID& event) override {
        stopped.push_back(event);
    }
    void SetAmbientVolume(const AudioEventID&, float) override {}
    void SetMasterVolume(float) override {}
    void SetMusicVolume(float) override {}
    void SetSfxVolume(float) override {}
    void SetBusVolume(BusId, float) override {}
    bool StopEvent(AudioEventHandle, bool) override {
        return true;
    }
    bool SetEventPosition(AudioEventHandle, const glm::vec3&) override {
        return true;
    }
    bool SetEventVolume(AudioEventHandle, float) override {
        return true;
    }
    bool SetEventParameter(AudioEventHandle, const AudioParamID&, float) override {
        return true;
    }
};

void ExpectFresh(const FrameAudioState& audio) {
    EXPECT_TRUE(audio.pendingThunder.empty());
    EXPECT_TRUE(audio.stride.empty());
    EXPECT_FALSE(audio.footInitialized);
    EXPECT_EQ(audio.footLastPos, glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(audio.footDist, 0.0f);
    EXPECT_FLOAT_EQ(audio.envTimer, 0.0f);
    EXPECT_FLOAT_EQ(audio.callTimer, 0.0f);
    EXPECT_FLOAT_EQ(audio.sleepTimer, 0.0f);
    EXPECT_FLOAT_EQ(audio.feedTimer, 0.0f);
    EXPECT_FLOAT_EQ(audio.colonyTimer, 0.0f);
    EXPECT_FLOAT_EQ(audio.thunderTimer, 0.0f);
    EXPECT_FALSE(audio.rainOn);
    EXPECT_FALSE(audio.waterOn);
    EXPECT_FALSE(audio.waterfallRoarOn);
    EXPECT_EQ(audio.waterfallRoarPosition, glm::vec3(0.0f));
    EXPECT_EQ(audio.sunUp, -1);
}

TEST(FrameAudioState, WorldReplacementDoesNotInheritReusedEntityStrideOrTimers) {
    FrameAudioState audio;
    std::uint32_t oldKey = 0;
    {
        entt::registry worldA;
        oldKey = entt::to_integral(worldA.create());
        audio.BeginCreatureFrame();
        EXPECT_FALSE(audio.CreatureFootfall(oldKey, {0.0f, 0.0f}, 1.7f));
        EXPECT_FALSE(audio.CreatureFootfall(oldKey, {1.5f, 0.0f}, 1.7f));
        EXPECT_FALSE(audio.PlayerFootstep({0.0f, 0.0f, 0.0f}, 0.5f));
        EXPECT_FALSE(audio.PlayerFootstep({1.5f, 0.0f, 0.0f}, 0.5f));
        audio.AdvanceTimers(0.49f);
        audio.thunderTimer = 21.5f;
        audio.sunUp = 1;
        audio.pendingThunder.push_back(
            {.strike_tick = 1, .fired_at_seconds = 0.0, .distance_m = 343.0f, .magnitude = 1.0f});
    }
    audio.Reset();
    ExpectFresh(audio);
    entt::registry worldB;
    const auto newKey = entt::to_integral(worldB.create());
    ASSERT_EQ(newKey, oldKey);
    // Old history would play a footfall immediately (1.5 + 0.5 >= 1.7).
    EXPECT_FALSE(audio.CreatureFootfall(newKey, {2.0f, 0.0f}, 1.7f));
    EXPECT_FALSE(audio.PlayerFootstep({2.0f, 0.0f, 0.0f}, 0.5f));
    audio.AdvanceTimers(0.02f);
    EXPECT_FLOAT_EQ(audio.envTimer, 0.02f);
    EXPECT_FLOAT_EQ(audio.callTimer, 0.02f);
    EXPECT_FLOAT_EQ(audio.sleepTimer, 0.02f);
    EXPECT_FLOAT_EQ(audio.feedTimer, 0.02f);
    EXPECT_FLOAT_EQ(audio.colonyTimer, 0.02f);
    EXPECT_FALSE(audio.CreatureFootfall(newKey, {3.5f, 0.0f}, 1.7f));
    EXPECT_TRUE(audio.CreatureFootfall(newKey, {4.0f, 0.0f}, 1.7f));
}

TEST(FrameAudioState, ResetStopsOnlyOwnedLoopsAndIsIdempotentWithOrWithoutBackend) {
    // All flag combinations include no loops, individual loops and all loops.
    for (unsigned flags = 0; flags < 8; ++flags) {
        for (const bool backend : {false, true}) {
            FrameAudioState audio;
            RecordingAudio manager;
            audio.rainOn = (flags & 1u) != 0;
            audio.waterOn = (flags & 2u) != 0;
            audio.waterfallRoarOn = (flags & 4u) != 0;
            audio.waterfallRoarPosition = glm::vec3(12.0f);
            std::vector<std::string> expected;
            if (backend && audio.rainOn)
                expected.emplace_back("ambient_rain");
            if (backend && audio.waterOn)
                expected.emplace_back("ambient_stream");
            if (backend && audio.waterfallRoarOn)
                expected.emplace_back("waterfall_roar");
            audio.Reset(backend ? &manager : nullptr);
            ExpectFresh(audio);
            EXPECT_EQ(manager.stopped, expected);
            audio.Reset(backend ? &manager : nullptr);
            ExpectFresh(audio);
            EXPECT_EQ(manager.stopped, expected);
        }
    }
}

TEST(FrameAudioState, PlayerCadenceRetainsSpeedGatesAndFirstEligibleFrameAnchoring) {
    FrameAudioState audio;
    EXPECT_FALSE(audio.PlayerFootstep({100.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_FALSE(audio.PlayerFootstep({101.0f, 20.0f, 0.0f}, 0.5f));
    EXPECT_TRUE(audio.PlayerFootstep({102.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_FLOAT_EQ(audio.footDist, 0.0f);
    EXPECT_FALSE(audio.PlayerFootstep({103.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_FALSE(audio.PlayerFootstep({103.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_FLOAT_EQ(audio.footDist, 0.0f);
    EXPECT_FALSE(audio.PlayerFootstep({200.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_FALSE(audio.PlayerFootstep({201.0f, 0.0f, 0.0f}, 0.0f));
    EXPECT_FALSE(audio.PlayerFootstep({202.0f, 0.0f, 0.0f}, -1.0f));
    EXPECT_FLOAT_EQ(audio.footDist, 0.0f);
    EXPECT_FALSE(audio.PlayerFootstep({203.0f, 0.0f, 0.0f}, 0.5f));
    EXPECT_TRUE(audio.PlayerFootstep({204.0f, 0.0f, 0.0f}, 0.5f));
}

TEST(FrameAudioState, CreatureGaitsRetainRemainderTeleportAndClearThreshold) {
    for (const float length : std::array{1.0f, 1.7f, 3.0f}) {
        FrameAudioState audio;
        EXPECT_FALSE(audio.CreatureFootfall(7, {0.0f, 0.0f}, length));
        EXPECT_FALSE(audio.CreatureFootfall(7, {0.5f, 0.0f}, length));
        EXPECT_FALSE(audio.CreatureFootfall(7, {10.0f, 0.0f}, length));
        EXPECT_FLOAT_EQ(audio.stride.at(7).second, 0.5f);
        EXPECT_TRUE(audio.CreatureFootfall(7, {10.0f + length, 0.0f}, length));
        EXPECT_NEAR(audio.stride.at(7).second, 0.5f, 0.00001f);
    }
    FrameAudioState audio;
    for (std::uint32_t key = 0; key < 512; ++key)
        EXPECT_FALSE(audio.CreatureFootfall(key, {0.0f, 0.0f}, 1.7f));
    audio.BeginCreatureFrame();
    EXPECT_EQ(audio.stride.size(), 512u);
    EXPECT_FALSE(audio.CreatureFootfall(512, {0.0f, 0.0f}, 1.7f));
    audio.BeginCreatureFrame();
    EXPECT_TRUE(audio.stride.empty());
}

TEST(FrameAudioState, IndependentOwnersNeverShareHistory) {
    FrameAudioState first;
    first.AdvanceTimers(10.0f);
    EXPECT_FALSE(first.CreatureFootfall(1, {0.0f, 0.0f}, 1.7f));
    FrameAudioState second;
    ExpectFresh(second);
    EXPECT_FLOAT_EQ(first.callTimer, 10.0f);
    EXPECT_EQ(first.stride.size(), 1u);
}
} // namespace
