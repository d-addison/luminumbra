#include "../support/UniqueTempPath.h"
#include "app/GraphicsSessionOverrides.h"
#include "core/RuntimeScenarioConfig.h"
#include "core/StrictNumeric.h"
#include "core/TimelapseVista.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Luminumbra::Client::ScenarioHarness::ParseRuntimeScenarioConfig;
using Luminumbra::Core::ParseNumber;

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

TEST(StrictNumericParsing, RefusesMalformedAndOutOfRangeNumbersWithoutAssignment) {
    for (const std::string text : {"12abc",
                                   "nan",
                                   "inf",
                                   " 12",
                                   "",
                                   "12 ",
                                   "\t12",
                                   "1e9999",
                                   "1e999",
                                   "+",
                                   "-",
                                   "++12",
                                   "+-12",
                                   "0x",
                                   "0x-1",
                                   "-0x",
                                   "0x1p2junk"}) {
        SCOPED_TRACE(text);
        int integer = 7;
        float single = 7.0f;
        double wide = 7.0;
        EXPECT_FALSE(ParseNumber(text, integer));
        EXPECT_FALSE(ParseNumber(text, single));
        EXPECT_FALSE(ParseNumber(text, wide));
        EXPECT_EQ(integer, 7);
        EXPECT_EQ(single, 7.0f);
        EXPECT_EQ(wide, 7.0);
    }
    for (const std::string text :
         {"1e-9999", "-1e-9999", "0x1p99999", "0x1p-99999", "NaN(payload)", "-infinity"}) {
        float single = 7.0f;
        double wide = 7.0;
        EXPECT_FALSE(ParseNumber(text, single)) << text;
        EXPECT_FALSE(ParseNumber(text, wide)) << text;
        EXPECT_EQ(single, 7.0f);
        EXPECT_EQ(wide, 7.0);
    }
    int integer = 7;
    double wide = 7.0;
    EXPECT_FALSE(ParseNumber("2147483648", integer));
    EXPECT_FALSE(ParseNumber("-2147483649", integer));
    EXPECT_FALSE(ParseNumber(std::string("12\0abc", 6), wide));
    EXPECT_EQ(integer, 7);
    EXPECT_EQ(wide, 7.0);
    uint64_t count = 7;
    for (const std::string text : {"-1", "18446744073709551616", "12abc", " 12", ""}) {
        EXPECT_FALSE(ParseNumber(text, count)) << text;
        EXPECT_EQ(count, 7);
    }
}

TEST(StrictNumericParsing, ValidNumbersMatchPreviousConversionsIncludingSignsAndHexFloats) {
    for (const std::string text : {"12", "+12", "-12", "0", "012", "2147483647", "-2147483648"}) {
        int parsed = 0;
        ASSERT_TRUE(ParseNumber(text, parsed)) << text;
        EXPECT_EQ(parsed, std::stoi(text));
    }
    for (const std::string text : {"12",
                                   "+12",
                                   "-12",
                                   "0",
                                   "-0",
                                   "+0.5",
                                   ".25",
                                   "12.",
                                   "1.25e-2",
                                   "3.4028234e38",
                                   "1.1754944e-38",
                                   "0x1.8p2",
                                   "-0X1.8p+2",
                                   "+0x1p-2"}) {
        float single = 0.0f;
        double wide = 0.0;
        ASSERT_TRUE(ParseNumber(text, single)) << text;
        ASSERT_TRUE(ParseNumber(text, wide)) << text;
        EXPECT_EQ(single, std::stof(text));
        EXPECT_EQ(wide, std::stod(text));
        EXPECT_EQ(std::signbit(single), std::signbit(std::stof(text)));
        EXPECT_EQ(std::signbit(wide), std::signbit(std::stod(text)));
    }
    uint64_t count = 0;
    ASSERT_TRUE(ParseNumber("18446744073709551615", count));
    EXPECT_EQ(count, std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(ParseNumber("+12", count));
    EXPECT_EQ(count, 12u);
}

TEST(StrictNumericParsing, RenderScaleOverrideKeepsSettingsOnFailureAndClampsValidValues) {
    for (const std::string text :
         {"0.5junk", "nan", "inf", "-inf", "", " 0.5", "0.5 ", "1e9999", "1e-9999"}) {
        SCOPED_TRACE(text);
        float scale = 0.0f;
        EXPECT_FALSE(ParseNumber(text, scale));
        EXPECT_EQ(scale, 0.0f);
        EXPECT_TRUE(Luminumbra::Client::GraphicsSessionOverrides("", scale, std::nullopt).empty());
    }
    for (const std::string text : {"0", "-0", "-0.5"}) {
        float scale = 1.0f;
        ASSERT_TRUE(ParseNumber(text, scale));
        EXPECT_TRUE(Luminumbra::Client::GraphicsSessionOverrides("", scale, std::nullopt).empty());
    }
    struct ScaleCase {
        const char* text;
        double expected;
    };
    constexpr ScaleCase cases[] = {
        {"0.25", 0.5}, {"0.5", 0.5}, {"+0.75", 0.75}, {"1", 1.0}, {"2", 1.0}, {"0x1p-1", 0.5}};
    for (const auto& test_case : cases) {
        float scale = 0.0f;
        ASSERT_TRUE(ParseNumber(test_case.text, scale));
        const auto overrides =
            Luminumbra::Client::GraphicsSessionOverrides("", scale, std::nullopt);
        ASSERT_EQ(overrides.count("render_scale"), 1u);
        const auto* actual_scale = std::get_if<double>(&overrides.at("render_scale"));
        ASSERT_NE(actual_scale, nullptr);
        EXPECT_EQ(*actual_scale, test_case.expected);
    }
}

TEST(StrictNumericParsing, MoonOverridesKeepTheirFallbacksOnFailureAndAcceptFiniteValues) {
    for (const std::string text : {"0.5junk",
                                   "nan",
                                   "NaN(payload)",
                                   "inf",
                                   "-inf",
                                   "",
                                   " 0.5",
                                   "0.5 ",
                                   "1e9999",
                                   "1e-9999"}) {
        SCOPED_TRACE(text);
        float illumination = -1.0f; // Select the configured illumination.
        float wrap_floor = 0.25f;
        EXPECT_FALSE(ParseNumber(text, illumination));
        EXPECT_FALSE(ParseNumber(text, wrap_floor));
        EXPECT_EQ(illumination, -1.0f);
        EXPECT_EQ(wrap_floor, 0.25f);
    }
    for (const std::string text : {"0", "0.5", "+0.75", "1", "2", "-1", "0x1p-1"}) {
        SCOPED_TRACE(text);
        float illumination = -1.0f;
        float wrap_floor = 0.25f;
        ASSERT_TRUE(ParseNumber(text, illumination));
        ASSERT_TRUE(ParseNumber(text, wrap_floor));
        EXPECT_EQ(illumination, std::stof(text));
        EXPECT_EQ(wrap_floor, std::stof(text));
    }
}

TEST(StrictNumericParsing, WorldGenSeedKeepsPreviousSeedOnFailureAndAcceptsIntBoundaries) {
    for (const std::string text : {"2147483648",
                                   "-2147483649",
                                   "9999999999999999999999",
                                   "0.5junk",
                                   "1337junk",
                                   "nan",
                                   "inf",
                                   "",
                                   "+",
                                   "-",
                                   " 12",
                                   "12 ",
                                   "1.5"}) {
        int seed = 1337;
        EXPECT_FALSE(ParseNumber(text, seed)) << text;
        EXPECT_EQ(seed, 1337) << text;
    }
    for (const std::string text : {"0", "42", "+42", "-42", "2147483647", "-2147483648"}) {
        int seed = 1337;
        ASSERT_TRUE(ParseNumber(text, seed)) << text;
        EXPECT_EQ(seed, std::stoi(text));
    }
}

TEST(RuntimeSurfaceOptionsTest, StrictScalarOptionsKeepTheirExistingFallbacksAndClamps) {
    namespace Harness = Luminumbra::Client::ScenarioHarness;
    for (const std::string text : {"12abc", "nan", "inf", " 12", "", "999999999999999999999999"}) {
        SCOPED_TRACE(text);
        std::vector<std::string> arguments{"client", "--number=" + text};
        std::vector<char*> argv;
        for (auto& argument : arguments)
            argv.push_back(argument.data());
        EXPECT_EQ(Harness::GetCommandLineIntOption(2, argv.data(), "--number", 9), 9);
        EXPECT_EQ(Harness::GetCommandLineUInt64Option(2, argv.data(), "--number", 9), 9u);
        const auto config =
            Parse({"client", "--foliage-density-scale=" + text, "--hang-watchdog-seconds=" + text});
        EXPECT_EQ(config.foliage_density_scale, 1.0f);
        EXPECT_EQ(config.hang_watchdog_seconds, 0);
    }
    const auto config = Parse({"client",
                               "--avatars=-2",
                               "--foliage-density-scale=2.5",
                               "--hang-watchdog-seconds=12",
                               "--memory-watermark-mb=+12"});
    EXPECT_EQ(config.avatars, 1); // Integer options retain their minimum-one clamp.
    EXPECT_EQ(config.foliage_density_scale, 2.5f);
    EXPECT_EQ(config.hang_watchdog_seconds, 12);
    EXPECT_EQ(config.memory_watermark_mb, 12u);
    EXPECT_EQ(Parse({"client", "--memory-watermark-mb=-1"}).memory_watermark_mb, 0u);
    EXPECT_EQ(Parse({"client", "--hang-watchdog-seconds=-1"}).hang_watchdog_seconds, 0);
    EXPECT_EQ(Parse({"client", "--foliage-density-scale=9"}).foliage_density_scale, 1.0f);
}

TEST(RuntimeSurfaceOptionsTest, StrictResolutionFailureKeepsBothDimensions) {
    const auto defaults = Parse({"client"});
    for (const std::string text : {"12abcx900",
                                   "1600x900abc",
                                   " 1600x900",
                                   "1600x 900",
                                   "",
                                   "nanx900",
                                   "1600xinf",
                                   "1600x",
                                   "x900",
                                   "1600",
                                   "0x900",
                                   "1600x0",
                                   "-1x900",
                                   "1600x99999999999999999999999",
                                   "1600x900x1"}) {
        const auto config = Parse({"client", "--resolution=" + text});
        EXPECT_EQ(config.windowed_width, defaults.windowed_width) << text;
        EXPECT_EQ(config.windowed_height, defaults.windowed_height) << text;
    }
    for (const std::string text : {"1600x900", "+1600X+900"}) {
        const auto config = Parse({"client", "--resolution=" + text});
        EXPECT_EQ(config.windowed_width, 1600);
        EXPECT_EQ(config.windowed_height, 900);
    }
}

TEST(StrictNumericParsing, TimelapsePanRefusesPrefixesAndKeepsItsClamp) {
    namespace Vista = Luminumbra::TimelapseVista;
    for (const std::string text : {"12abc", "nan", "inf", " 12", "", "1e9999"})
        EXPECT_EQ(Vista::ParsePanDegrees(text), std::nullopt) << text;
    EXPECT_EQ(Vista::ParsePanDegrees("+0.5"), std::optional<float>(0.5f));
    EXPECT_EQ(Vista::ParsePanDegrees("-12"), std::optional<float>(-2.0f));
    EXPECT_EQ(Vista::ParsePanDegrees("12"), std::optional<float>(2.0f));
}

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
