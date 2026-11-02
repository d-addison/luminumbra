#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Luminumbra::Client::ScenarioHarness::ParseRuntimeScenarioConfig;

auto Parse(std::vector<std::string> arguments, const fs::path& root = {}) {
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return ParseRuntimeScenarioConfig(static_cast<int>(argv.size()), argv.data(), root);
}

auto ParsePaths(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return Luminumbra::Client::ScenarioHarness::ParseRuntimePathOptions(
        static_cast<int>(argv.size()), argv.data());
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path = Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra-runtime-surface-");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

TEST(RuntimeSurfaceOptionsTest, RefusesBeforeCreatingSaveRootInEitherArgumentOrder) {
    const TemporaryDirectory directory;
    const auto save_root = directory.path / "absent";
    for (const std::string flag :
         {"--horizon-radius", "--collision-radius", "--render-benchmark-stream-radius"}) {
        for (const std::string value : {"", "-1", "13", "10000", "1junk", "999999999999999999"}) {
            for (const bool radius_first : {false, true}) {
                for (const bool equals : {false, true}) {
                    const std::vector<std::string> radius_arguments =
                        equals
                            ? std::vector<std::string>{std::string(flag).append("=").append(value)}
                            : std::vector<std::string>{flag, value};
                    std::vector<std::string> arguments{"client"};
                    if (radius_first)
                        arguments.insert(
                            arguments.end(), radius_arguments.begin(), radius_arguments.end());
                    arguments.insert(arguments.end(), {"--save-root", save_root.string()});
                    if (!radius_first)
                        arguments.insert(
                            arguments.end(), radius_arguments.begin(), radius_arguments.end());
                    EXPECT_THROW(ParsePaths(arguments), std::invalid_argument);
                    EXPECT_FALSE(fs::exists(save_root));
                }
            }
        }
    }
    for (const std::string schema : {"v2", "v3", "v5", ""}) {
        for (const bool assigned : {false, true}) {
            std::vector<std::string> arguments{
                "client", "--save-root", save_root.string(), "--render-benchmark-schema=" + schema};
            if (assigned)
                arguments.emplace_back("--render-benchmark-stream-radius=0");
            else
                arguments.insert(arguments.end(), {"--render-benchmark-stream-radius", "1"});
            EXPECT_THROW(ParsePaths(arguments), std::invalid_argument);
            EXPECT_FALSE(fs::exists(save_root)) << "schema refusal created the save root";
        }
    }
    EXPECT_THROW(
        ParsePaths(
            {"client", "--save-root", save_root.string(), "--render-benchmark-stream-radius", "0"}),
        std::invalid_argument);
    EXPECT_FALSE(fs::exists(save_root)) << "stream-radius refusal created the save root";
    const auto accepted = ParsePaths({"client",
                                      "--save-root",
                                      save_root.string(),
                                      "--horizon-radius=12",
                                      "--collision-radius",
                                      "0"});
    EXPECT_EQ(accepted.save_root, save_root);
    EXPECT_TRUE(fs::is_directory(save_root));
}

TEST(RuntimeSurfaceOptionsTest, RefusesInvalidRadiiAndAcceptsBoundariesInBothSpellings) {
    for (const std::string flag :
         {"--horizon-radius", "--collision-radius", "--render-benchmark-stream-radius"}) {
        EXPECT_THROW(Parse({"client", flag}), std::invalid_argument);
        for (const std::string value : {"",
                                        "-1",
                                        "13",
                                        "10000",
                                        "2147483647",
                                        "4294967295",
                                        "99999999999999999999999999",
                                        "1junk",
                                        "0junk"}) {
            EXPECT_THROW(Parse({"client", flag, value}), std::invalid_argument);
            EXPECT_THROW(Parse({"client", std::string(flag).append("=").append(value)}),
                         std::invalid_argument);
        }
        for (const std::string value : {"0", "4", "12"}) {
            const auto separate = Parse({"client", "--render-benchmark-schema=v4", flag, value});
            const auto assigned = Parse({"client",
                                         "--render-benchmark-schema=v4",
                                         std::string(flag).append("=").append(value)});
            for (const auto* parsed : {&separate, &assigned}) {
                EXPECT_EQ(parsed->horizon_radius,
                          flag == "--horizon-radius" ? std::stoi(value) : 12);
                EXPECT_EQ(parsed->collision_radius,
                          flag == "--collision-radius" ? std::stoi(value) : 4);
                EXPECT_EQ(parsed->benchmark_stream_radius,
                          flag == "--render-benchmark-stream-radius" ? std::stoi(value) : 0);
            }
        }
        EXPECT_THROW(Parse({"client", flag, "13", flag, "4"}), std::invalid_argument);
        EXPECT_THROW(Parse({"client", flag, "4", flag, "13"}), std::invalid_argument);
    }
    EXPECT_EQ(Parse({"client"}).horizon_radius, 12);
    EXPECT_EQ(Parse({"client"}).collision_radius, 4);
    EXPECT_EQ(Parse({"client"}).benchmark_stream_radius, 0);
}

} // namespace
