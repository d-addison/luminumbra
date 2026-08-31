// dead-audio-weight prune guard. Two jobs, both
// GL/device-free (headers are only compiled + type-trait-inspected; NO
// MiniaudioManager is constructed and no ma_* / GL call is ever made):
//
//   (1) COMPILE SENTINEL — force every SURVIVING audio header through the
//       compiler so a future accidental dependency on one of the 5 orphan
//       systems  deleted (AdvancedReverbSystem / ProceduralSoundGenerator
//       / SoundVariationSystem / AudioStreamingManager / AudioPerformanceProfiler)
//       fails HERE, loudly, instead of in the client link.
//
//   (2) MANIFEST GUARD — the 7 dormant bank manifests  retired
//       (adaptive_music_system, complex_creature_sfx, complex_environmental_sfx,
//       creature_sfx, environmental_sfx, sound_variation_definitions,
//       weather_sfx — 221 orphaned.ogg refs, never LoadBank'd) must be GONE
//       from disk, and the loaded-bank set must stay exactly {sfx_main, music},
//       the only two banks main_client.cpp ever LoadBank's.
//
// Data-driven at test time via LUMINUMBRA_SOURCE_ROOT (same idiom as the sibling
// AudioBankIntegrity_test.cpp).
#include <gtest/gtest.h>

// --- (1) surviving-header compile sentinel ---------------------------------
#include "audio/AudioSpatialCluster.h"
#include "audio/EnvironmentalAudioModel.h"
#include "audio/IAudioManager.h"
#include "audio/MiniaudioManager.h"
#include "audio/MixerModel.h"

#include <filesystem>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

// Compile-time proof the surviving public API is intact. Each trait needs a
// COMPLETE type, so any of these lines fails to compile if the header stopped
// building (e.g. it had leaned on one of the now-deleted orphan systems).
static_assert(std::is_abstract_v<Luminumbra::Client::IAudioManager>,
              "IAudioManager must remain the abstract audio interface");
static_assert(
    std::is_base_of_v<Luminumbra::Client::IAudioManager, Luminumbra::Client::MiniaudioManager>,
    "MiniaudioManager must remain an IAudioManager implementation");
static_assert(std::is_class_v<Luminumbra::Client::AudioSpatialCluster>,
              "AudioSpatialCluster must remain a defined type");
static_assert(std::is_class_v<Luminumbra::Client::Audio::MixerDucker>,
              "MixerModel's MixerDucker must remain a defined type");
static_assert(sizeof(Luminumbra::Client::Audio::DuckParams) > 0,
              "MixerModel's DuckParams must remain a defined type");
// EnvironmentalAudioModel is pure-std math: prove its gating curve is still
// callable at compile time (constexpr edge) — anchors the header's inclusion.
static_assert(Luminumbra::Client::AudioModel::kDayEdgeSinElevation > 0.0f,
              "EnvironmentalAudioModel's day-edge constant must remain defined");

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

const fs::path kRoot = fs::path(LUMINUMBRA_SOURCE_ROOT);

// Mirrors main_client.cpp's TWO LoadBank calls — the only banks loaded at
// runtime; every other manifest under data/audio was dormant authoring data.
const std::vector<std::string> kLoadedBanks = {"sfx_main", "music"};

// The 7 dormant manifests  retired.
const std::vector<std::string> kPrunedBanks = {"adaptive_music_system",
                                               "complex_creature_sfx",
                                               "complex_environmental_sfx",
                                               "creature_sfx",
                                               "environmental_sfx",
                                               "sound_variation_definitions",
                                               "weather_sfx"};

fs::path BankPath(const std::string& name) {
    return kRoot / "data" / "audio" / (name + ".bank.json");
}

} // namespace

// (2a) Set check (documentation-grade): no retired bank name leaks into the
// loaded-bank set, which stays exactly {sfx_main, music}.
TEST(AudioPruneGuard, PrunedBanksNotInLoadedSet) {
    const std::set<std::string> loaded(kLoadedBanks.begin(), kLoadedBanks.end());
    for (const std::string& pruned : kPrunedBanks) {
        EXPECT_EQ(loaded.count(pruned), 0u)
            << "retired bank '" << pruned << "' must not be in the loaded set";
    }
    EXPECT_EQ(loaded.size(), 2u) << "exactly sfx_main + music are loaded";
}

// (2b) On-disk check (load-bearing — the only assertion that proves the prune
// actually happened): the 7 retired manifests are gone; the two loaded ones
// survive.
TEST(AudioPruneGuard, RetiredBankManifestsDeletedLoadedSurvive) {
    const fs::path audio_dir = kRoot / "data" / "audio";
    ASSERT_TRUE(fs::exists(audio_dir))
        << "data/audio not found under LUMINUMBRA_SOURCE_ROOT=" << kRoot.string();

    for (const std::string& pruned : kPrunedBanks) {
        EXPECT_FALSE(fs::exists(BankPath(pruned)))
            << "retired bank manifest still on disk ( prune incomplete): "
            << BankPath(pruned).string();
    }
    for (const std::string& loaded : kLoadedBanks) {
        EXPECT_TRUE(fs::exists(BankPath(loaded)))
            << "a LOADED bank manifest is missing: " << BankPath(loaded).string();
    }
}
