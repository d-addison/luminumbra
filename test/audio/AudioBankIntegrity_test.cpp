// Bank-integrity and format checks for the audio event manifests.
//
//   (a) every file referenced by the LOADED banks (sfx_main + music — the only
//       two main_client.cpp ever loads) exists on disk;
//   (b) loaded banks are ogg-free: this miniaudio build has NO ogg decoder
//       (ogg -> MA_INVALID_FILE -> SILENT playback), so the extension
//       allowlist is {mp3, wav};
//   (c) every audio event-id literal at a client playback call site resolves
//       to an event in a loaded bank (unknown ids fail SILENTLY at runtime by
//       design — this test is where they fail loudly instead);
//   (d) the one dynamic event family ("creature_" + species_id + "_call",
//       built from the species registry) resolves for EVERY authored species —
//       forward coverage: adding a species without its call event goes RED.
//
// Data-driven-at-test-time via LUMINUMBRA_SOURCE_ROOT, the same idiom as
// BiomeReverb_test.cpp.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

const fs::path kRoot = fs::path(LUMINUMBRA_SOURCE_ROOT);

// The authoritative loaded-bank set (mirrors main_client.cpp's two LoadBank
// calls; every other *.bank.json under data/audio is dormant authoring data).
const std::vector<fs::path> kLoadedBanks = {
    fs::path("data") / "audio" / "sfx_main.bank.json",
    fs::path("data") / "audio" / "music.bank.json",
};

nlohmann::json LoadJson(const fs::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input.is_open()) << "cannot open " << path.string();
    nlohmann::json j;
    input >> j;
    return j;
}

// Merged event-id set across the loaded banks.
std::set<std::string> LoadedEventIds() {
    std::set<std::string> ids;
    for (const fs::path& bank : kLoadedBanks) {
        const nlohmann::json j = LoadJson(kRoot / bank);
        for (const auto& [id, def] : j.at("events").items()) {
            ids.insert(id);
        }
    }
    return ids;
}

// Plausible event id: lowercase snake_case, no path separators/extensions.
bool LooksLikeEventId(const std::string& s) {
    static const std::regex kIdRe("^[a-z][a-z0-9_]+$");
    return std::regex_match(s, kIdRe);
}

} // namespace

TEST(AudioBankIntegrity, LoadedBankFilesExistOnDisk) {
    const fs::path audio_root = kRoot / "assets" / "audio";
    if (!fs::exists(audio_root)) {
        GTEST_SKIP() << "audio binaries are intentionally external to the repository";
    }

    for (const fs::path& bank : kLoadedBanks) {
        const fs::path bank_path = kRoot / bank;
        ASSERT_TRUE(fs::exists(bank_path)) << bank_path.string();
        const nlohmann::json j = LoadJson(bank_path);
        for (const auto& [id, def] : j.at("events").items()) {
            const auto& files = def.at("files");
            ASSERT_TRUE(files.is_array() && !files.empty())
                << bank.string() << " event '" << id << "' has no files";
            for (const auto& file : files) {
                const fs::path asset = kRoot / file.get<std::string>();
                EXPECT_TRUE(fs::exists(asset))
                    << bank.string() << " event '" << id
                    << "' references a missing asset: " << asset.string();
            }
        }
    }
}

TEST(AudioBankIntegrity, LoadedBanksAreOggFree) {
    for (const fs::path& bank : kLoadedBanks) {
        const nlohmann::json j = LoadJson(kRoot / bank);
        for (const auto& [id, def] : j.at("events").items()) {
            for (const auto& file : def.at("files")) {
                fs::path asset(file.get<std::string>());
                std::string ext = asset.extension().string();
                for (char& c : ext)
                    c = static_cast<char>(std::tolower(c));
                EXPECT_TRUE(ext == ".mp3" || ext == ".wav")
                    << bank.string() << " event '" << id << "' file '" << asset.string()
                    << "' has extension '" << ext
                    << "' — this miniaudio build decodes only mp3/wav (ogg is "
                       "MA_INVALID_FILE -> silent)";
            }
        }
    }
}

TEST(AudioBankIntegrity, ClientEventLiteralsResolveInLoadedBanks) {
    const std::set<std::string> events = LoadedEventIds();
    ASSERT_FALSE(events.empty());

    // Playback entry points whose first quoted argument is an event id.
    static const std::regex kCallRe(
        "(PlayOneShot2D|PlayOneShot|PlayMusic|PlayAmbientLoop|StopAmbientLoop|"
        "SetAmbientVolume|PlayEvent)\\s*\\(");
    static const std::regex kLiteralRe("\"([a-z][a-z0-9_]+)\"");

    std::size_t literals_checked = 0;
    std::vector<std::string> unresolved;
    for (const auto& entry :
         fs::recursive_directory_iterator(kRoot / "src" / "luminumbra_client")) {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream input(entry.path());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline(input, line)) {
            ++line_no;
            // Strip line comments so documentation mentions never count.
            if (const auto comment = line.find("//"); comment != std::string::npos) {
                line.resize(comment);
            }
            std::smatch call;
            if (!std::regex_search(line, call, kCallRe))
                continue;
            // Every plausible event-id literal in the call's argument tail.
            const std::string tail = call.suffix().str();
            for (auto it = std::sregex_iterator(tail.begin(), tail.end(), kLiteralRe);
                 it != std::sregex_iterator();
                 ++it) {
                const std::string id = (*it)[1].str();
                if (!LooksLikeEventId(id))
                    continue;
                ++literals_checked;
                if (events.count(id) == 0u) {
                    unresolved.push_back(id + " (" + entry.path().filename().string() + ":" +
                                         std::to_string(line_no) + ")");
                }
            }
        }
    }
    // Coverage sanity: the client currently plays ~40 distinct literals; a scan
    // that suddenly sees almost none means the scanner (not the game) broke.
    EXPECT_GE(literals_checked, 25u)
        << "the call-site scan found suspiciously few event literals — scanner broken?";
    EXPECT_TRUE(unresolved.empty())
        << unresolved.size() << " event literal(s) do not resolve in the loaded banks "
        << "(they fail SILENTLY at runtime): " << [&unresolved] {
               std::string joined;
               for (const auto& u : unresolved)
                   joined += "\n  " + u;
               return joined;
           }();
}

TEST(AudioBankIntegrity, EverySpeciesHasACallEvent) {
    // main_client.cpp builds "creature_" + species_id + "_call" dynamically from
    // the species registry — invisible to the literal scan above, so enumerate
    // the registry and assert the whole family resolves.
    const std::set<std::string> events = LoadedEventIds();
    const fs::path species_dir = kRoot / "data" / "common" / "creatures" / "species";
    ASSERT_TRUE(fs::exists(species_dir)) << species_dir.string();
    std::size_t species_checked = 0;
    for (const auto& entry : fs::directory_iterator(species_dir)) {
        if (entry.path().extension() != ".json")
            continue;
        const nlohmann::json j = LoadJson(entry.path());
        if (!j.contains("id"))
            continue;
        const std::string call_event = "creature_" + j.at("id").get<std::string>() + "_call";
        EXPECT_EQ(events.count(call_event), 1u)
            << entry.path().filename().string() << " has no '" << call_event
            << "' event in the loaded banks (its call plays SILENTLY)";
        ++species_checked;
    }
    EXPECT_GE(species_checked, 5u) << "species registry scan found suspiciously few species";
}

TEST(AudioBankIntegrity, CoreWorldStateEventsResolve) {
    const std::set<std::string> loaded = LoadedEventIds();
    std::vector<std::string> unresolved;
    for (const char* id : {
             "ambient_birds",
             "ambient_night",
             "creature_sleep",
             "creature_feed",
             "creature_drink",
             "creature_colony",
         }) {
        if (loaded.count(id) == 0u)
            unresolved.push_back(id);
    }

    EXPECT_TRUE(unresolved.empty())
        << "core world-state event ids are absent from loaded banks: " << [&unresolved] {
               std::string joined;
               for (const auto& id : unresolved)
                   joined += "\n  " + id;
               return joined;
           }();
}
