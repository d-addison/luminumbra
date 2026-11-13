#include "../support/RegionFixtureCapturePublication.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using Luminumbra::TestSupport::PublishCapture;

TEST(RegionFixtureCapture, PublicationRefusesExistingDestinationAndCleansStaging) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    const std::string original = "previous capture\n";
    {
        std::ofstream output(destination, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << original;
    }
    try {
        PublishCapture(destination, "replacement capture\n");
        FAIL() << "publication must refuse an existing destination";
    } catch (const fs::filesystem_error& error) {
        EXPECT_NE(std::string(error.what()).find("destination must be new"), std::string::npos);
    }
    std::ifstream input(destination, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    EXPECT_EQ(contents.str(), original);
    // The destination is the only remaining entry; no reserved staging directory.
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

TEST(RegionFixtureCapture, PublicationCreatesNewDestinationAndCleansStaging) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    const std::string payload = "new capture\n";
    PublishCapture(destination, payload);
    std::ifstream input(destination, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    EXPECT_EQ(contents.str(), payload);
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

TEST(RegionFixtureCapture, PublicationFailuresLeaveNoStagingOrDestination) {
    const Luminumbra::TestSupport::ScopedTempFile file("capture_publish_", ".json");
    const auto& destination = file.Path();
    // A missing parent refuses staging without leaving a directory behind.
    EXPECT_THROW(PublishCapture(destination / "missing/capture.json", "capture\n"),
                 fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(destination.parent_path()));
    // An existing directory is also an occupied publication name.
    fs::create_directory(destination);
    EXPECT_THROW(PublishCapture(destination, "capture\n"), fs::filesystem_error);
    EXPECT_TRUE(fs::is_empty(destination));
    for (const auto& entry : fs::directory_iterator(destination.parent_path())) {
        EXPECT_EQ(entry.path(), destination);
    }
}

} // namespace
