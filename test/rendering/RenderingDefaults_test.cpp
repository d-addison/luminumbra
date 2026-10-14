// Pure rendering override policy: no client library or GL context required.
#include "luminumbra_client/rendering/RenderingDefaults.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <locale>
#include <string>

// glibc oracle verified with stdlib Python/ctypes (C locale):
// libc = ctypes.CDLL(None); libc.sscanf.restype = ctypes.c_int
// Pass ctypes.byref(c_float(default)) / byref(c_int(default)) for each field.
// "%f,%f,%f,%d,%f":
//   "0,1e,0.2,12,2" -> 5 assignments: (0, 1, 0.2, 12, 2)
//   "0,1e+,0.2,12,2" -> the same 5 assignments
// "%f%n,%f" (the %n output measures consumption, not an assignment):
//   "infi,2" -> 0 assignments, first field unchanged
//   "1e+,2" -> 2 assignments: (1, 2), first consumed length 3
//   "0x1p2,0x.8p+1" -> 2 assignments: (4, 1)
//   "1 ,2" -> 1 assignment; "1, 2" -> 2 assignments
//   "1e-9999,2" -> 2 assignments: (0, 2)
// "%d,%f": "+12,2" and "012,2" -> (12, 2), 2 assignments;
//   "12e,2" -> 1 assignment; "0x12,2" -> 1 assignment to integer 0.
// glibc accepts Inf/NaN/float overflow. Rendering deliberately rejects them.
// Integer overflow is undefined for scanf; these helpers reject it as well.
// Hex floats are retained because the old %f accepted them (C99 syntax).

using Luminumbra::Rendering::ParseCommaSeparatedPrefix;

TEST(RenderDefaults, ShippingPresentationAndExplicitOff) {
    using namespace Luminumbra::Rendering;
    EXPECT_EQ(kDefaultVolumetricQuality, 1);
    EXPECT_FLOAT_EQ(kDefaultAetherMaterialModulation, 1.0f);
    EXPECT_TRUE(ScentDecalEnabled(std::nullopt));
    EXPECT_FALSE(ScentDecalEnabled(std::string("0")));
    EXPECT_TRUE(ScentDecalEnabled(std::string("1")));
    const auto defaults = ResolveCaveAmbientOcclusion(std::nullopt);
    EXPECT_FLOAT_EQ(defaults.enabled, 1.0f);
    EXPECT_FLOAT_EQ(defaults.maxDist, 24.0f);
    EXPECT_FLOAT_EQ(defaults.floor, 0.06f);
    EXPECT_EQ(defaults.steps, 8);
    EXPECT_FLOAT_EQ(defaults.thickness, 1.5f);
    const auto off = ResolveCaveAmbientOcclusion(std::string("0"));
    EXPECT_FLOAT_EQ(off.enabled, 0.0f);
    EXPECT_FLOAT_EQ(off.maxDist, defaults.maxDist);
    EXPECT_EQ(off.steps, defaults.steps);
    const auto tuned = ResolveCaveAmbientOcclusion(std::string("1,30,0.1,12,2"));
    EXPECT_FLOAT_EQ(tuned.maxDist, 30.0f);
    EXPECT_EQ(tuned.steps, 12);
    EXPECT_FLOAT_EQ(tuned.thickness, 2.0f);
}

TEST(RenderDefaults, CaveAmbientOcclusionPreservesPartialAndInvalidOverrides) {
    using namespace Luminumbra::Rendering;
    const auto expect = [](const std::string& input, const CaveAmbientOcclusion& expected) {
        SCOPED_TRACE(input);
        const auto actual = ResolveCaveAmbientOcclusion(input);
        EXPECT_FLOAT_EQ(actual.enabled, expected.enabled);
        EXPECT_FLOAT_EQ(actual.maxDist, expected.maxDist);
        EXPECT_FLOAT_EQ(actual.floor, expected.floor);
        EXPECT_EQ(actual.steps, expected.steps);
        EXPECT_FLOAT_EQ(actual.thickness, expected.thickness);
    };
    expect("", {});
    expect("invalid", {});
    expect("0,invalid,0.2,12,2", {0.0f, 24.0f, 0.06f, 1.5f, 8});
    expect("0,30,invalid,12,2", {0.0f, 30.0f, 0.06f, 1.5f, 8});
    expect("0,30,0.2,invalid,2", {0.0f, 30.0f, 0.2f, 1.5f, 8});
    expect("0,30,0.2,12,invalid", {0.0f, 30.0f, 0.2f, 1.5f, 12});
    expect("0,30,0.2,12", {0.0f, 30.0f, 0.2f, 1.5f, 12});
    expect("0,30,0.2,12.5,2", {0.0f, 30.0f, 0.2f, 1.5f, 12});
    expect("0 ,30,0.2,12,2", {0.0f, 24.0f, 0.06f, 1.5f, 8});
    expect("0,,0.2,12,2", {0.0f, 24.0f, 0.06f, 1.5f, 8});
    expect(" +0, 3e1, 0.2, +12, 2suffix", {0.0f, 30.0f, 0.2f, 2.0f, 12});
    expect("0,0x1ep0,0.2,-12,2,extra", {0.0f, 30.0f, 0.2f, 2.0f, -12});
    expect("0,1e,0.2,12,2", {0.0f, 1.0f, 0.2f, 2.0f, 12});
    expect("0,1e+,0.2,12,2", {0.0f, 1.0f, 0.2f, 2.0f, 12});
    expect("0,nan,0.2,12,2", {0.0f, 24.0f, 0.06f, 1.5f, 8});
}

TEST(RenderDefaults, PartialInfinityRetainsGradeExposure) {
    std::array<float, 6> grade{1.12f, 1.14f, 1.06f, 1.06f, 1.0f, 0.92f};
    const auto defaults = grade;
    EXPECT_EQ(ParseCommaSeparatedPrefix(
                  "infi", grade[0], grade[1], grade[2], grade[3], grade[4], grade[5]),
              0u);
    EXPECT_EQ(grade, defaults);
}

TEST(RenderDefaults, FloatMatchingConsumesIncompleteExponents) {
    for (const auto* input : {"1e,2", "1e+,2", "1e-,2", "1E+,2", "0x1p,2", "0x1p-,2"}) {
        SCOPED_TRACE(input);
        float first = 9.0f, second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 2u);
        EXPECT_FLOAT_EQ(first, 1.0f);
        EXPECT_FLOAT_EQ(second, 2.0f);
    }
    for (const auto* input : {"1eX,2", "1e+-2,2"}) {
        SCOPED_TRACE(input);
        float first = 9.0f, second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 1u);
        EXPECT_FLOAT_EQ(first, 1.0f);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
}

TEST(RenderDefaults, ValidShortAndWhitespaceSeparatedPrefixes) {
    float first = 9.0f, second = 10.0f, third = 11.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix(" \t\n\r\f\v+1.25, -2.5, .5suffix", first, second, third),
              3u);
    EXPECT_FLOAT_EQ(first, 1.25f);
    EXPECT_FLOAT_EQ(second, -2.5f);
    EXPECT_FLOAT_EQ(third, 0.5f);

    first = 9.0f;
    second = 10.0f;
    third = 11.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix("2", first, second, third), 1u);
    EXPECT_FLOAT_EQ(first, 2.0f);
    EXPECT_FLOAT_EQ(second, 10.0f);
    EXPECT_FLOAT_EQ(third, 11.0f);

    for (const auto* input : {"1 ,2", "1\t,2", "1;2", "1,,2", "1,", "1, "}) {
        SCOPED_TRACE(input);
        first = 9.0f;
        second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 1u);
        EXPECT_FLOAT_EQ(first, 1.0f);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
}

TEST(RenderDefaults, ZeroAndHexPrefixAtTerminatorPreserveDefaults) {
    for (const auto* input : {"0", "-0", "+0"}) {
        SCOPED_TRACE(input);
        float first = 9.0f, second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 1u);
        EXPECT_FLOAT_EQ(first, 0.0f);
        EXPECT_EQ(std::signbit(first), input[0] == '-');
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
    // A hex prefix without significand digits is refused before assignment.
    float first = 9.0f, second = 10.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix("0x", first, second), 0u);
    EXPECT_FLOAT_EQ(first, 9.0f);
    EXPECT_FLOAT_EQ(second, 10.0f);
}

TEST(RenderDefaults, InvalidOrNonFiniteFieldStopsBeforeAssignment) {
    for (const auto* input : {"",
                              " ",
                              ".",
                              "+",
                              "-",
                              "0x",
                              "infi",
                              "inf",
                              "INFINITY",
                              "-inf",
                              "+NaN",
                              "nan(payload)",
                              "1e9999",
                              "0x1p128"}) {
        SCOPED_TRACE(input);
        float first = 9.0f, second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 0u);
        EXPECT_FLOAT_EQ(first, 9.0f);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
    for (const auto* token : {"inf", "-INFINITY", "nan", "1e9999", "-0x1p9999"}) {
        SCOPED_TRACE(token);
        float first = 9.0f, second = 10.0f, third = 11.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(std::string("1,") + token + ",3", first, second, third),
                  1u);
        EXPECT_FLOAT_EQ(first, 1.0f);
        EXPECT_FLOAT_EQ(second, 10.0f);
        EXPECT_FLOAT_EQ(third, 11.0f);
    }
}

TEST(RenderDefaults, HexFloatsAndFiniteRangeEdgesRemainSupported) {
    float first = 0.0f, second = 0.0f, third = 0.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix("0x1p2, -0X.8P+1, +0x1.8", first, second, third), 3u);
    EXPECT_FLOAT_EQ(first, 4.0f);
    EXPECT_FLOAT_EQ(second, -1.0f);
    EXPECT_FLOAT_EQ(third, 1.5f);
    EXPECT_EQ(ParseCommaSeparatedPrefix("0x1.fffffep127,0x1p-149", first, second), 2u);
    EXPECT_EQ(first, std::numeric_limits<float>::max());
    EXPECT_EQ(second, std::numeric_limits<float>::denorm_min());
}

TEST(RenderDefaults, UnderflowAndZeroWithHugeExponentAreSuccessful) {
    for (const auto* token : {"1e-9999", "1e-46", "0x1p-9999", "0e9999", "0x0p9999"}) {
        SCOPED_TRACE(token);
        float first = 9.0f, second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(std::string(token) + ",2", first, second), 2u);
        EXPECT_FLOAT_EQ(first, 0.0f);
        EXPECT_FALSE(std::signbit(first));
        EXPECT_FLOAT_EQ(second, 2.0f);
        EXPECT_EQ(ParseCommaSeparatedPrefix(std::string("-") + token + ",2", first, second), 2u);
        EXPECT_FLOAT_EQ(first, 0.0f);
        EXPECT_TRUE(std::signbit(first));
    }
}

TEST(RenderDefaults, DecimalIntegerMatchingAndFailurePreserveFollowingDefaults) {
    for (const auto* input : {" \t+12,2", "012,2"}) {
        SCOPED_TRACE(input);
        int first = 9;
        float second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 2u);
        EXPECT_EQ(first, 12);
        EXPECT_FLOAT_EQ(second, 2.0f);
    }
    for (const auto* input : {"12e,2", "12.5,2", "12 ,2", "12"}) {
        SCOPED_TRACE(input);
        int first = 9;
        float second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 1u);
        EXPECT_EQ(first, 12);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
    int first = 9;
    float second = 10.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix("0x12,2", first, second), 1u);
    EXPECT_EQ(first, 0);
    EXPECT_FLOAT_EQ(second, 10.0f);

    for (const auto* input : {"", "+,2", "-,2", "+-12,2", "--12,2"}) {
        SCOPED_TRACE(input);
        first = 9;
        EXPECT_EQ(ParseCommaSeparatedPrefix(input, first, second), 0u);
        EXPECT_EQ(first, 9);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
    for (const int limit : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        SCOPED_TRACE(limit);
        first = 9;
        EXPECT_EQ(ParseCommaSeparatedPrefix(std::to_string(limit) + ",2", first, second), 2u);
        EXPECT_EQ(first, limit);
        EXPECT_FLOAT_EQ(second, 2.0f);
        first = 9;
        second = 10.0f;
        EXPECT_EQ(ParseCommaSeparatedPrefix(std::to_string(limit) + "0,2", first, second), 0u);
        EXPECT_EQ(first, 9);
        EXPECT_FLOAT_EQ(second, 10.0f);
    }
}

namespace {

class CommaDecimal : public std::numpunct<char> {
    char do_decimal_point() const override {
        return ',';
    }
};

class ScopedCommaLocale {
public:
    ScopedCommaLocale()
        : previous(std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))) {}
    ~ScopedCommaLocale() {
        std::locale::global(previous);
    }

private:
    std::locale previous;
};

} // namespace

TEST(RenderDefaults, DecimalSyntaxIgnoresGlobalLocale) {
    [[maybe_unused]] const ScopedCommaLocale locale;
    float first = 9.0f, second = 10.0f;
    EXPECT_EQ(ParseCommaSeparatedPrefix("1.5,2.5", first, second), 2u);
    EXPECT_FLOAT_EQ(first, 1.5f);
    EXPECT_FLOAT_EQ(second, 2.5f);
}
