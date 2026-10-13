#include "luminumbra_client/rendering/FarWorldSurfaceTag.h"

#include <bit>
#include <cstdint>
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

namespace {
using namespace Luminumbra::Rendering;

TEST(FarWorldSurfaceTag, EndpointsSurviveBinary16WithAnUnambiguousSign) {
    for (const bool far : {false, true}) {
        for (const float ao : {0.0f, 1.0f}) {
            const auto packed = glm::packHalf2x16({0.25f, EncodeFarWorldSurfaceAO(ao, far)});
            const auto stored = glm::unpackHalf2x16(packed);
            EXPECT_EQ(IsFarWorldSurface(stored.y), far);
            EXPECT_FLOAT_EQ(DecodeFarWorldSurfaceAO(stored.y), ao);
            EXPECT_FLOAT_EQ(stored.x, 0.25f);
            const std::uint32_t expected = far ? (ao == 0.0f ? 0xbc00u : 0xc000u)
                                              : (ao == 0.0f ? 0x0000u : 0x3c00u);
            EXPECT_EQ(packed >> 16, expected);
        }
    }
    EXPECT_FALSE(IsFarWorldSurface(-0.0f));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(DecodeFarWorldSurfaceAO(-0.0f)), 0x80000000u);
}

TEST(FarWorldSurfaceTag, DenseAORoundTripThroughActualHalfFloatQuantization) {
    // The signed range [-2,-1] has a 1/1024 step. Test finer input steps,
    // including halfway cases, independently of exact binary16 endpoints.
    for (int sample = 0; sample <= 8192; ++sample) {
        const float ao = static_cast<float>(sample) / 8192.0f;
        const auto ordinary_bits = glm::packHalf2x16({0.37f, ao});
        for (const bool far : {false, true}) {
            const auto bits = glm::packHalf2x16({0.37f, EncodeFarWorldSurfaceAO(ao, far)});
            const auto stored = glm::unpackHalf2x16(bits);
            EXPECT_EQ(IsFarWorldSurface(stored.y), far) << sample;
            EXPECT_NEAR(DecodeFarWorldSurfaceAO(stored.y), ao, 1.0f / 2048.0f) << sample;
            EXPECT_EQ(bits & 0xffffu, ordinary_bits & 0xffffu); // Metallic is unchanged.
            if (!far)
                EXPECT_EQ(bits, ordinary_bits);
        }
    }
}

TEST(FarWorldSurfaceTag, EveryOrdinaryBinary16AOValueIsABitwiseNoOp) {
    // All nonnegative binary16 values from zero through one, including subnormals.
    for (std::uint32_t bits = 0; bits <= 0x3c00u; ++bits) {
        const float ao = glm::unpackHalf2x16(bits).x;
        EXPECT_FALSE(IsFarWorldSurface(ao));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(EncodeFarWorldSurfaceAO(ao, false)),
                  std::bit_cast<std::uint32_t>(ao));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(DecodeFarWorldSurfaceAO(ao)),
                  std::bit_cast<std::uint32_t>(ao));
    }
}
} // namespace
