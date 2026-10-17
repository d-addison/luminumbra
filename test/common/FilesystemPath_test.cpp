#include "../support/UniqueTempPath.h"
#include "luminumbra_client/app/RuntimeRoot.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#if !defined(_WIN32) || defined(__CYGWIN__)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace paths = Luminumbra::Filesystem;

namespace {
struct InputFileRoot {
    fs::path path = Luminumbra::TestSupport::CreateUniqueTempDirectory("input-file-");
    ~InputFileRoot() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
} // namespace

TEST(FilesystemPath, InputFileRetainsOpenedImageAcrossReplacementAndMove) {
    const InputFileRoot root;
    const auto original = root.path / "image";
    const auto replacement = root.path / "replacement";
    std::ofstream(original, std::ios::binary) << "old";
    std::ofstream(replacement, std::ios::binary) << "newer";
    paths::ReplacementSafeInputFile input;
    ASSERT_TRUE(input.open(original));
    std::error_code error;
    EXPECT_EQ(input.size(error), 3u);
    EXPECT_FALSE(error);
    ASSERT_TRUE(fs::remove(original));
    fs::rename(replacement, original);
    paths::ReplacementSafeInputFile moved(std::move(input));
    char bytes[3]{};
    ASSERT_TRUE(moved.read(bytes, sizeof(bytes)));
    EXPECT_EQ(std::string(bytes, sizeof(bytes)), "old");
    EXPECT_FALSE(moved.read(bytes, 1));
    EXPECT_FALSE(moved.seek(std::numeric_limits<std::uint64_t>::max()));
    ASSERT_TRUE(moved.seek(0));
    ASSERT_TRUE(moved.read(bytes, sizeof(bytes)));
    EXPECT_EQ(std::string(bytes, sizeof(bytes)), "old");
    paths::ReplacementSafeInputFile assigned;
    assigned = std::move(moved);
    ASSERT_TRUE(assigned.seek(0));
    ASSERT_TRUE(assigned.read(bytes, sizeof(bytes)));
    EXPECT_EQ(std::string(bytes, sizeof(bytes)), "old");
    ASSERT_TRUE(assigned.open(original));
    EXPECT_EQ(assigned.size(error), 5u);
    EXPECT_FALSE(error);
    EXPECT_FALSE(assigned.open(root.path / "missing"));
    EXPECT_FALSE(assigned.is_open());
    EXPECT_EQ(assigned.size(error), 0u);
    EXPECT_EQ(error, std::make_error_code(std::errc::bad_file_descriptor));
    EXPECT_FALSE(assigned.read(bytes, sizeof(bytes)));
    EXPECT_FALSE(assigned.seek(0));
}

TEST(FilesystemPath, InputFileRefusesDirectoriesDevicesAndSpecialFiles) {
    const InputFileRoot root;
    paths::ReplacementSafeInputFile input;
    EXPECT_FALSE(input.open(root.path));
    EXPECT_FALSE(input.is_open());
#if defined(_WIN32) && !defined(__CYGWIN__)
    EXPECT_FALSE(input.open(L"NUL"));
#else
    EXPECT_FALSE(input.open("/dev/null"));
    const auto fifo = root.path / "fifo";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    // There is deliberately no writer: a blocking open would hang this test.
    EXPECT_FALSE(input.open(fifo));
#endif
    EXPECT_FALSE(input.is_open());
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
    const fs::path directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("runtime-root-");
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{directory};
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
