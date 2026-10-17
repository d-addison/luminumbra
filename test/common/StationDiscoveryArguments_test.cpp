#include "../../tools/StationDiscoveryArguments.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {
using namespace Luminumbra::StationDiscoveryCli;

TEST(StationDiscoveryArguments, NativePathsPreserveUnicodeAndSpaces) {
#ifdef _WIN32
    constexpr auto argument = L"C:\\Users\\\u96ea\\station \U0001f30d\\preset.json";
    const std::u8string expected = u8"C:\\Users\\\u96ea\\station \U0001f30d\\preset.json";
#else
    constexpr auto argument = "/users/\xe9\x9b\xaa/station \xf0\x9f\x8c\x8d/preset.json";
    const std::u8string expected = u8"/users/\u96ea/station \U0001f30d/preset.json";
#endif
    const auto path = ArgumentPath(argument);
    EXPECT_EQ(path.native(), argument);
    EXPECT_EQ(path.u8string(), expected);
}

TEST(StationDiscoveryArguments, TextPreservesSwitchesKindsAndSignedIntegers) {
    for (const auto* text : {"--preset-json", "coverage", "-4000", "2147483615"}) {
        const auto native = std::filesystem::path(text).native();
        EXPECT_EQ(ArgumentText(native), text);
    }
}

#ifdef _WIN32
TEST(StationDiscoveryArguments, NonAsciiTextIsRefusedWithoutLossyConversion) {
    EXPECT_THROW(ArgumentText(L"\u96ea"), UsageError);
    EXPECT_THROW(ArgumentText(L"\u0131"), UsageError);
}
#else
TEST(StationDiscoveryArguments, PosixArgumentsPreserveArbitraryBytes) {
    const std::string argument = "station-\xff";
    EXPECT_EQ(ArgumentText(argument), argument);
    EXPECT_EQ(ArgumentPath(argument).native(), argument);
}
#endif
} // namespace
