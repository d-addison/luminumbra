#include "../support/UniqueTempPath.h"
#include "luminumbra_client/app/RuntimeRoot.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/FilesystemPathDiagnostics.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
namespace paths = Luminumbra::Filesystem;

TEST(FilesystemPath, OrdinaryLongNamesDoNotProbeForExpansion) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    const fs::path root(LR"(C:\content with long names)");
#else
    const fs::path root("/content with long names");
#endif
    // These spellings need no existing files, short-name support or link privilege.
    for (const auto* suffix : {"data/asset.json",
                               "DATA/ASSET.JSON",
                               "data/missing/asset.json",
                               "data/<invalid>",
                               "data/file.json/child"}) {
        auto input = root / suffix;
        input.make_preferred();
        ASSERT_EQ(input.native().find(fs::path::value_type('~')), fs::path::string_type::npos);
        auto error = std::make_error_code(std::errc::permission_denied);
        const auto before = paths::LongNameExpansionCount();
        EXPECT_EQ(paths::LongNamePath(input, error).native(), input.native());
        EXPECT_FALSE(error);
        EXPECT_EQ(paths::LongNameExpansionCount(), before);
    }
#if defined(_WIN32) && !defined(__CYGWIN__)
    for (const fs::path& input : {fs::path(LR"(\\?\C:\content with long names\data\missing)"),
                                  fs::path(LR"(\\?\UNC\server\share\data\missing)")}) {
        const auto expected = input.native().starts_with(LR"(\\?\UNC\)")
                                  ? fs::path(LR"(\\server\share\data\missing)")
                                  : root / "data/missing";
        std::error_code error;
        const auto before = paths::LongNameExpansionCount();
        EXPECT_TRUE(paths::PathSpellingEqual(paths::LongNamePath(input, error), expected));
        EXPECT_FALSE(error);
        EXPECT_EQ(paths::LongNameExpansionCount(), before);
    }
#endif
}

TEST(FilesystemPath, LongNameExpansionPreservesEmptyAndOverlongPathErrors) {
    for (const fs::path& input :
         {fs::path{}, fs::path(std::string(32768, 'x')), fs::path(std::string(32767, 'x') + '~')}) {
        auto error = std::make_error_code(std::errc::permission_denied);
        const auto before = paths::LongNameExpansionCount();
        const auto expanded = paths::LongNamePath(input, error);
#if defined(_WIN32) && !defined(__CYGWIN__)
        EXPECT_TRUE(expanded.empty());
        EXPECT_EQ(error,
                  input.empty() ? std::errc::no_such_file_or_directory
                                : std::errc::filename_too_long);
#else
        EXPECT_EQ(expanded.native(), input.native());
        EXPECT_FALSE(error);
#endif
        EXPECT_EQ(paths::LongNameExpansionCount(), before);
    }
}

TEST(FilesystemPath, TildeNamesStillExpandAndRetainMissingSuffixes) {
    const Luminumbra::TestSupport::ScopedTempFile temp("long-name-", ".tmp");
    const auto directory = temp.Path().parent_path();
    const auto tilde = directory / "literal~name";
    ASSERT_TRUE(fs::create_directory(tilde));
    std::error_code error;
    const auto expanded_directory = paths::LongNamePath(directory, error);
    ASSERT_FALSE(error);
    for (const fs::path& input : {tilde, tilde / "missing/asset.json"}) {
        const auto expected = expanded_directory / input.lexically_relative(directory);
        const auto before = paths::LongNameExpansionCount();
        EXPECT_TRUE(paths::PathSpellingEqual(paths::LongNamePath(input, error), expected));
        EXPECT_FALSE(error);
#if defined(_WIN32) && !defined(__CYGWIN__)
        EXPECT_GT(paths::LongNameExpansionCount(), before);
#else
        EXPECT_EQ(paths::LongNameExpansionCount(), before);
#endif
    }
}

TEST(FilesystemPath, OrdinaryPathsKeepStandardNormalizationAndClassification) {
    for (const fs::path& input : {fs::path("."),
                                  fs::path("res/shaders/../shaders/g_buffer.vert"),
                                  fs::path("../data/common/./biomes.json")}) {
        EXPECT_FALSE(paths::IsWindowsUncPath(input));
        EXPECT_EQ(paths::IsAbsolutePath(input), input.is_absolute());
        EXPECT_EQ(paths::AbsolutePath(input).native(), fs::absolute(input).native());
        EXPECT_EQ(paths::LexicallyNormalPath(input).native(), input.lexically_normal().native());
        EXPECT_EQ(paths::GenericPathString(input), input.generic_string());
    }
    std::error_code expected_error, actual_error;
    const auto expected = fs::absolute(fs::path{}, expected_error);
    const auto actual = paths::AbsolutePath(fs::path{}, actual_error);
    EXPECT_EQ(actual.native(), expected.native());
    EXPECT_EQ(actual_error, expected_error);
}

TEST(FilesystemPath, NormalizedSourcePathsReadActualShadersAndBiomes) {
    const fs::path root(LUMINUMBRA_SOURCE_ROOT);
    for (const char* relative :
         {"res/shaders/../shaders/g_buffer.vert", "data/common/./biomes.json"}) {
        const fs::path normalized =
            paths::LexicallyNormalPath(paths::AbsolutePath(root / relative));
        ASSERT_TRUE(paths::IsAbsolutePath(normalized));
        std::ifstream input(normalized, std::ios::binary);
        ASSERT_TRUE(input.is_open()) << paths::GenericPathString(normalized);
        EXPECT_NE(input.peek(), std::ifstream::traits_type::eof());
    }
}

TEST(FilesystemPath, RuntimeRootStringKeepsOrdinaryPlatformSpelling) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    const fs::path root("C:/assets/runtime");
#else
    const fs::path root("/assets/runtime");
#endif
    std::string expected = root.generic_string();
    if (expected.empty() || expected.back() != '/') {
        expected.push_back('/');
    }
    EXPECT_EQ(Luminumbra::Client::App::RuntimeRootString(root), expected);
}

TEST(FilesystemPath, ExplicitRuntimeRootBypassesDiscoveryAndRefusesIncompleteLayouts) {
    const Luminumbra::TestSupport::ScopedTempFile temp("runtime-root-", ".tmp");
    const fs::path directory = temp.Path().parent_path();
    const auto resolve = [&](const fs::path& root) {
        return Luminumbra::Client::App::ResolveRuntimeRoot("client", root);
    };
    EXPECT_THROW(resolve("relative"), std::invalid_argument);
    EXPECT_THROW(resolve(directory / "missing"), std::invalid_argument);
    EXPECT_THROW(resolve(directory), std::invalid_argument);
    for (const char* sentinel : {"res/shaders/g_buffer.vert",
                                 "data/audio/music.bank.json",
                                 "worlds/atlas/presets/default.json"}) {
        const auto path = directory / sentinel;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
    }
    EXPECT_EQ(resolve(directory), directory);
    for (const char* sentinel : {"res/shaders/g_buffer.vert",
                                 "data/audio/music.bank.json",
                                 "worlds/atlas/presets/default.json"}) {
        const auto path = directory / sentinel;
        fs::remove(path);
        EXPECT_THROW(resolve(directory), std::invalid_argument);
        fs::create_directory(path);
        EXPECT_THROW(resolve(directory), std::invalid_argument);
        fs::remove(path);
        std::ofstream(path) << "fixture";
    }
    EXPECT_THROW(resolve(directory / "res/shaders/g_buffer.vert"), std::invalid_argument);
}

#if defined(_WIN32) && !defined(__CYGWIN__)
TEST(FilesystemPath, UncDataRootNormalizationKeepsDependencyPathsOnTheShare) {
    for (const fs::path& input : {fs::path(LR"(//server/share/data with spaces/./nested/..)"),
                                  fs::path(LR"(\\server\share\data with spaces\.\nested\..)"),
                                  fs::path(LR"(//server/share\data with spaces/nested\..)")}) {
        const auto root = paths::LexicallyNormalPath(paths::AbsolutePath(input));
        EXPECT_EQ(root.native(), fs::path(LR"(\\server\share\data with spaces)").native());
        const auto dependency = root / "common" / "biomes.json";
        EXPECT_TRUE(paths::IsWindowsUncPath(dependency));
        EXPECT_EQ(paths::GenericUtf8PathString(dependency),
                  "//server/share/data with spaces/common/biomes.json");
    }
}

TEST(FilesystemPath, UncNormalizationPreservesServerShareAcrossSlashForms) {
    const fs::path expected(LR"(\\server\share\folder with spaces\shader.vert)");
    for (const fs::path& input :
         {fs::path(LR"(//server/share/folder with spaces/./nested/../shader.vert)"),
          fs::path(LR"(\\server\share\folder with spaces\.\nested\..\shader.vert)"),
          fs::path(LR"(//server/share\folder with spaces/nested\../shader.vert)")}) {
        ASSERT_TRUE(paths::IsWindowsUncPath(input));
        ASSERT_TRUE(paths::IsAbsolutePath(input));
        const auto normalized = paths::LexicallyNormalPath(paths::AbsolutePath(input));
        EXPECT_EQ(normalized.native(), expected.native());
        EXPECT_TRUE(paths::IsAbsolutePath(normalized));
        EXPECT_EQ(paths::GenericPathString(normalized),
                  "//server/share/folder with spaces/shader.vert");
        EXPECT_EQ(paths::LexicallyNormalPath(normalized.parent_path() / "sibling.vert").native(),
                  fs::path(LR"(\\server\share\folder with spaces\sibling.vert)").native());
    }
}

TEST(FilesystemPath, UncDotDotCannotConsumeTheShareRoot) {
    const auto path =
        paths::LexicallyNormalPath(fs::path(LR"(\\server\share\folder\..\..\shader.vert)"));
    EXPECT_EQ(path.native(), fs::path(LR"(\\server\share\shader.vert)").native());
    EXPECT_TRUE(paths::IsAbsolutePath(path));
    auto ancestor = paths::LexicallyNormalPath(fs::path(LR"(\\server\share\folder\child)"));
    for (int i = 0; i < 8; ++i) {
        ancestor = paths::LexicallyNormalPath(ancestor / "..");
    }
    std::string root = paths::GenericPathString(ancestor);
    if (root.back() == '/') {
        root.pop_back();
    }
    EXPECT_EQ(root, "//server/share");
    EXPECT_EQ(paths::LexicallyNormalPath(ancestor / "..").native(), ancestor.native());
}

TEST(FilesystemPath, UncNormalizationKeepsUnicodeAndRuntimeRootPrefix) {
    const fs::path root(L"\\\\server\\share\\caf\u00e9\\.\\folder\\..");
    const auto normalized = paths::LexicallyNormalPath(root);
    EXPECT_EQ(normalized.native(), fs::path(L"\\\\server\\share\\caf\u00e9").native());
    EXPECT_EQ(Luminumbra::Client::App::RuntimeRootString(fs::path(LR"(\\server\share\assets)")),
              "//server/share/assets/");
}

TEST(FilesystemPath, WindowsRootRelativeAndDriveRelativePathsAreNotUncOrAbsolute) {
    for (const fs::path& input : {fs::path(LR"(\server\share)"),
                                  fs::path(L"C:relative"),
                                  fs::path(L"//"),
                                  fs::path(L"relative\\file")}) {
        EXPECT_FALSE(paths::IsWindowsUncPath(input));
        EXPECT_FALSE(paths::IsAbsolutePath(input));
    }
    EXPECT_TRUE(paths::IsAbsolutePath(fs::path(LR"(C:\assets\shader.vert)")));
}
#endif
