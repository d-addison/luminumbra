#include "UniqueTempPath.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using Luminumbra::TestSupport::ScopedTempFile;
} // namespace

TEST(UniqueTempPath, SamePrefixFilesHaveDistinctReservedDirectoriesAndCleanUp) {
    fs::path first_directory;
    fs::path second_directory;
    {
        const ScopedTempFile first("luminumbra-temp-path-", ".json");
        const ScopedTempFile second("luminumbra-temp-path-", ".json");
        first_directory = first.Path().parent_path();
        second_directory = second.Path().parent_path();

        EXPECT_NE(first.Path(), second.Path());
        EXPECT_NE(first_directory, second_directory);
        ASSERT_TRUE(fs::is_directory(first_directory));
        ASSERT_TRUE(fs::is_directory(second_directory));
        EXPECT_FALSE(fs::exists(first.Path()));
        EXPECT_FALSE(fs::exists(second.Path()));
        EXPECT_EQ(first.Path().extension(), fs::path(".json"));
        EXPECT_EQ(second.Path().extension(), fs::path(".json"));

        std::ofstream output(first.Path());
        ASSERT_TRUE(output.is_open());
        output << "first";
        ASSERT_TRUE(output.good());
        EXPECT_FALSE(fs::exists(second.Path()));
    }
    EXPECT_FALSE(fs::exists(first_directory));
    EXPECT_FALSE(fs::exists(second_directory));
}

TEST(UniqueTempPath, ReservationSkipsPreExistingFileAndReservedDirectoryCandidates) {
    // An owned workspace isolates the deterministic candidates and cleans up
    // both the blocker and all reserved directories on fatal assertions too.
    const ScopedTempFile workspace("luminumbra-temp-collision-", ".unused");
    const auto root = workspace.Path().parent_path();
    const auto occupied = root / "candidate_0";
    {
        std::ofstream blocker(occupied);
        ASSERT_TRUE(blocker.is_open());
        blocker << "keep";
        ASSERT_TRUE(blocker.good());
    }
    ASSERT_TRUE(fs::is_regular_file(occupied));

    // Exercise the same atomic reservation used by UniqueTempFilePath, with
    // identical stems to force collisions without relying on clock timing.
    std::error_code error;
    const auto first =
        Luminumbra::TestSupport::detail::ReserveTempDirectory(root, "candidate", error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_EQ(first, root / "candidate_1");
    ASSERT_TRUE(fs::is_directory(first));

    const auto second =
        Luminumbra::TestSupport::detail::ReserveTempDirectory(root, "candidate", error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(second, root / "candidate_2");
    EXPECT_NE(first, second);
    EXPECT_NE(first, occupied);
    EXPECT_NE(second, occupied);
    EXPECT_TRUE(fs::is_directory(second));
    EXPECT_TRUE(fs::is_regular_file(occupied));

    std::ifstream blocker(occupied);
    ASSERT_TRUE(blocker.is_open());
    std::string contents;
    std::getline(blocker, contents);
    EXPECT_EQ(contents, "keep");
}
