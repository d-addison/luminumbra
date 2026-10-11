#include "luminumbra_client/app/GameAssets.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using namespace Luminumbra::Client::App;

class GameAssets : public ::testing::Test {
protected:
    fs::path root = fs::temp_directory_path() /
                    ("luminumbra-game-assets-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    nlohmann::json manifest;

    void SetUp() override {
        fs::create_directories(root);
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void write(const fs::path& relative, const std::string& contents) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream out(root / relative, std::ios::binary);
        out << contents;
        ASSERT_TRUE(out.good());
    }
    void writeManifest() {
        write("config/game-asset-packs.json", manifest.dump());
    }
    void makePack() {
        auto files = nlohmann::json::array();
        auto add = [&](const std::string& relative) {
            const auto path = fs::path(kTreePackDirectory) / relative;
            write(path, "fixture");
            files.push_back(
                {{"path", relative}, {"size", 7}, {"sha256", AssetFileSha256(root / path)}});
        };
        for (const std::string part : {"trunk", "branches", "leaves"})
            for (const std::string lod : {"", ".lod1", ".lod2"})
                add("data/models/trees/tree_small_02_" + part + lod + ".lmesh");
        for (const std::string part : {"trunk", "branch", "leaves"})
            for (const std::string map : {"albedo", "normal", "arm"})
                add("data/textures/models/tree_" + part + "_" + map + "_512.ltex");
        manifest = {
            {"schema", "luminumbra.game.asset-packs.v1"},
            {"packs",
             {{"tree-small-02-runtime",
               {{"version", "1.0.0"}, {"install_dir", kTreePackDirectory}, {"files", files}}}}}};
        writeManifest();
    }
};

TEST_F(GameAssets, Sha256MatchesPublishedVectorsIncludingPaddingBoundaries) {
    const std::pair<std::string, std::string> cases[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for (const auto& [input, expected] : cases) {
        write("digest-input", input);
        EXPECT_EQ(AssetFileSha256(root / "digest-input"), expected);
    }
}

TEST_F(GameAssets, CompletePackVerifiesAndSameLengthCorruptionIsRefused) {
    makePack();
    EXPECT_TRUE(VerifyGameAssets(root).empty());
    const fs::path mesh =
        fs::path(kTreePackDirectory) / "data/models/trees/tree_small_02_trunk.lmesh";
    write(mesh, "changed");
    const auto refusal = VerifyGameAssets(root);
    EXPECT_NE(refusal.find("corrupt"), std::string::npos);
    EXPECT_NE(refusal.find("tools/assets/acquire.py"), std::string::npos);
    std::ifstream stream(root / mesh);
    std::string bytes;
    stream >> bytes;
    EXPECT_EQ(bytes, "changed");
}

TEST_F(GameAssets, MissingManifestOrLodCannotBecomeAValidPack) {
    EXPECT_FALSE(VerifyGameAssets(root).empty());
    makePack();
    auto& files = manifest["packs"]["tree-small-02-runtime"]["files"];
    files[1]["path"] = "extra";
    write(fs::path(kTreePackDirectory) / "extra", "fixture");
    writeManifest();
    EXPECT_NE(VerifyGameAssets(root).find("omits a required tree mesh LOD"), std::string::npos);
}

TEST_F(GameAssets, FuturePackAndTraversalAreRefused) {
    makePack();
    auto& pack = manifest["packs"]["tree-small-02-runtime"];
    pack["version"] = "2.0.0";
    writeManifest();
    EXPECT_NE(VerifyGameAssets(root).find("Unsupported"), std::string::npos);
    pack["version"] = "1.0.0";
    pack["files"][0]["path"] = "../escape";
    writeManifest();
    EXPECT_NE(VerifyGameAssets(root).find("Invalid path"), std::string::npos);
}

TEST_F(GameAssets, RedirectedPackAncestorCannotVerifyOutsideContent) {
    makePack();
    const auto source = root / "game-assets";
    const auto outside = root / "outside";
    fs::rename(source, outside);
#ifdef _WIN32
    // NTFS junction creation works without privileged symbolic-link permission.
    ASSERT_EQ(root.wstring().find_first_of(L"\"%\r\n"), std::wstring::npos);
    const auto command = L"cmd.exe /d /c mklink /j \"" + source.wstring() + L"\" \"" +
                         outside.wstring() + L"\" >nul";
    ASSERT_EQ(_wsystem(command.c_str()), 0);
#else
    fs::create_directory_symlink(outside, source);
#endif
    const auto refusal = VerifyGameAssets(root);
    EXPECT_NE(refusal.find("symbolic link or reparse point"), std::string::npos);
    fs::remove(source);
    fs::rename(outside, source);
    EXPECT_TRUE(VerifyGameAssets(root).empty());
}
} // namespace
