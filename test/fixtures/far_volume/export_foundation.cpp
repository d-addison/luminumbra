// Compile only against the pinned foundation headers, never the canonical facade.
#include "FarLodStore.h"
#include "FarVolume.h"
#include "FoundationFixture.h"
#include "core/Crc32.h"
#include <bit>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace Luminumbra;
using namespace Luminumbra::World;
namespace Fixture = FarVolumeFoundationFixture;
void Mix(Core::Crc32Accumulator& crc, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        const auto b = static_cast<std::uint8_t>(value >> (8 * i));
        crc.Update(&b, 1);
    }
}
int main() {
    nlohmann::json cases = nlohmann::json::array();
    for (std::uint32_t tier = 1; tier <= 5; ++tier)
        for (unsigned mode = 0; mode < 2; ++mode) {
            const unsigned field = mode ? 1 : 0;
            FarVolumeRequest request{tier, -2, -1, {}, static_cast<FarCaveMode>(mode)};
            if (tier == 1)
                request.extra_span = FarVolumeSpan{-512.5f, -400};
            if (tier == 2)
                request.extra_span = FarVolumeSpan{400, 400};
            const auto d = *FarTierAt(tier);
            Core::Crc32Accumulator heights, encoded, materials;
            std::uint64_t height_count = 0, density_count = 0;
            const auto tile = BuildFarVolumeTile(
                request,
                {[&](float x, float z) {
                     const auto h = Fixture::Height(x, z);
                     Mix(heights, std::bit_cast<std::uint32_t>(h), 4);
                     ++height_count;
                     return h;
                 },
                 [&](const Vec3& p, float h) {
                     const auto density = Fixture::Density(p.y, h, field);
                     const auto material = Fixture::Material(static_cast<std::int64_t>(p.x),
                                                             static_cast<std::int64_t>(p.y),
                                                             static_cast<std::int64_t>(p.z),
                                                             d.sample_spacing_meters);
                     Mix(encoded, static_cast<std::uint16_t>(QuantizeFarLodSdf(density)), 2);
                     Mix(encoded, material, 1);
                     Mix(materials, material, 1);
                     ++density_count;
                     return FarVolumeDensity{density, material};
                 }});
            ValidateFarVolumeTile(tile);
            nlohmann::json entry = {{"tier", tier},
                                    {"tile_x", request.tile_x},
                                    {"tile_z", request.tile_z},
                                    {"caves", mode},
                                    {"field", field},
                                    {"first_brick_y", tile.first_brick_y},
                                    {"last_brick_y", tile.last_brick_y},
                                    {"candidates", tile.sampled_bricks},
                                    {"sparse_bricks", tile.bricks.size()},
                                    {"tile_crc32", tile.crc32},
                                    {"height_count", height_count},
                                    {"density_count", density_count},
                                    {"height_crc32", heights.Value()},
                                    {"encoded_lattice_crc32", encoded.Value()},
                                    {"material_lattice_crc32", materials.Value()}};
            entry["extra_span"] =
                request.extra_span
                    ? nlohmann::json::array({request.extra_span->min_y, request.extra_span->max_y})
                    : nlohmann::json(nullptr);
            cases.push_back(entry);
        }
    std::cout << cases.dump(2) << '\n';
}
