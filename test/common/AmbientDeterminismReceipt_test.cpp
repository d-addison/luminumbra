// Emits one machine-readable ambient determinism receipt line per run. CTest captures stdout in the
// JUnit system-out, and tools/ci/compare_ambient_receipts.py compares the lines across CI lanes.
// The values are NOT pinned here: pins are added only after the receipts agree across all lanes.
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include "luminumbra_common/systems/AmbientNoiseDispatch.h"

#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

namespace {

using Luminumbra::Vec3;
using Luminumbra::Systems::AetherFieldSystem;
using Luminumbra::Systems::WeatherSystem;
using Luminumbra::Systems::WindFieldSystem;

constexpr int kSeed = 424242;
constexpr std::uint64_t kReceiptTicks = 64;
const Vec3 kAnchor(8.0f, 100.0f, 8.0f);

bool HasNoWhitespace(const std::string& s) {
    return !s.empty() && s.find_first_of(" \t\r\n") == std::string::npos;
}

TEST(AmbientDeterminismReceipt, EmitsConstructorAndEvolvedHashes) {
    WindFieldSystem wind(kSeed);
    WeatherSystem weather(kSeed);
    AetherFieldSystem aether(kSeed);
    const std::string wind_ctor = wind.ComputeWindSubHash();
    const std::string weather_ctor = weather.ComputeWeatherSubHash();
    const std::string aether_ctor = aether.ComputeAetherSubHash();
    for (std::uint64_t tick = 1; tick <= kReceiptTicks; ++tick) {
        wind.Update(tick, kAnchor);
        weather.Update(tick, kAnchor, &wind);
        aether.Update(tick, kAnchor, &wind);
    }
    const std::string wind_evolved = wind.ComputeWindSubHash();
    const std::string weather_evolved = weather.ComputeWeatherSubHash();
    const std::string aether_evolved = aether.ComputeAetherSubHash();
    for (const std::string* hash : {&wind_ctor,
                                    &weather_ctor,
                                    &aether_ctor,
                                    &wind_evolved,
                                    &weather_evolved,
                                    &aether_evolved}) {
        EXPECT_TRUE(HasNoWhitespace(*hash)) << *hash;
    }
    EXPECT_NE(wind_ctor, wind_evolved);
    EXPECT_NE(weather_ctor, weather_evolved);
    EXPECT_NE(aether_ctor, aether_evolved);

    std::ostringstream line;
    line << "AMBIENT_RECEIPT version=1 seed=" << kSeed << " ticks=" << kReceiptTicks
         << " ceiling=" << static_cast<int>(Luminumbra::Systems::kAmbientNoiseMaxSIMDLevel)
         << " cpu_simd=" << static_cast<int>(FastSIMD::CPUMaxSIMDLevel())
         << " wind_ctor=" << wind_ctor << " wind_evolved=" << wind_evolved
         << " weather_ctor=" << weather_ctor << " weather_evolved=" << weather_evolved
         << " aether_ctor=" << aether_ctor << " aether_evolved=" << aether_evolved;
    std::cout << line.str() << std::endl;
    RecordProperty("ambient_receipt", line.str());
}

} // namespace
