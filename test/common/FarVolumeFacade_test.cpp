#include "luminumbra_common/world/FarDensityQuantization.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/FarVolumeMesher.h"
#include "luminumbra_common/world/FarVolumeTile.h"

#include "../fixtures/far_volume/FoundationFixture.h"
#include "luminumbra_common/core/Crc32.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <type_traits>

namespace {
using namespace Luminumbra::World;

static_assert(std::is_same_v<decltype(FarVolumeSample{}.density), float>);
static_assert(std::is_same_v<decltype(FarVolumeTile{}.request), FarVolumeRequest>);
static_assert(std::is_same_v<decltype(FarVolumeMesh{}.request), FarVolumeRequest>);
static_assert(std::is_same_v<decltype(FarVolumeVertex{}.position), FarVolumeVector>);

FarVolumeCompilationLimits FixtureLimits() {
    // Explicit analytic-fixture bounds; not production queue/storage targets.
    return {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}};
}
FarVolumeAuthority
Plane(FarVolumeNumericProfile profile = FarVolumeNumericProfile::LegacySigned16) {
    return {42,
            FarCaveMode::BandLimited,
            profile,
            [](std::int64_t, std::int64_t) { return 0.0f; },
            [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                return FarVolumeSample{static_cast<float>(p.y) + .25f, 7};
            }};
}

TEST(FarVolumeFacade, ResolvesEveryTierNegativeOriginsAndExactTopPlane) {
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        SCOPED_TRACE(tier);
        const auto dimensions = *FarTierAt(tier);
        FarVolumeRequest request;
        FarVolumeFacadeFailure failure;
        ASSERT_TRUE(ResolveFarVolumeCoverage(
            {{tier, -2, -1}, {}}, Plane(), FixtureLimits().authority, request, &failure));
        EXPECT_EQ(request.key, (FarVolumeTileKey{tier, -2, -1}));
        EXPECT_EQ(request.span.min_y, -256);
        EXPECT_EQ(request.span.max_y, dimensions.brick_edge_meters);
        EXPECT_EQ(request.field_identity, 42u);
        EXPECT_EQ(failure.work.height_samples, 129u * 129u);
        EXPECT_EQ(failure.work.density_samples, 0u);
    }
}

TEST(FarVolumeFacade, CoverageExtensionsUnionTheWholeSurfaceBand) {
    auto authority = Plane();
    authority.height = [](std::int64_t x, std::int64_t z) {
        return 16.0f + static_cast<float>(x) / 64 + static_cast<float>(z) / 128;
    };
    FarVolumeRequest request;
    ASSERT_TRUE(ResolveFarVolumeCoverage({{1, 0, 0}, FarVolumeCoverageSpan{-1024, -900}},
                                         authority,
                                         FixtureLimits().authority,
                                         request));
    EXPECT_EQ(request.span, (FarVolumeSpan{-1024, 32}));
    ASSERT_TRUE(ResolveFarVolumeCoverage({{1, 0, 0}, FarVolumeCoverageSpan{1000, 1000}},
                                         authority,
                                         FixtureLimits().authority,
                                         request));
    EXPECT_EQ(request.span, (FarVolumeSpan{-240, 1008}));
    auto limits = FixtureLimits();
    limits.generation.max_candidate_bricks = 1024;
    FarVolumeCompilation prior;
    prior.intent.key = {4, 2, 3};
    const auto held = prior;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolume(
        {{1, 0, 0}, FarVolumeCoverageSpan{-1024, -900}}, authority, limits, prior, &failure));
    EXPECT_EQ(failure.generation, FarVolumeBuildError::WorkLimit);
    EXPECT_EQ(failure.work.density_samples, 0u);
    EXPECT_EQ(prior, held);
}

TEST(FarVolumeFacade, RejectsBadInputsBeforeAnyCallbackAndPreservesOutput) {
    auto authority = Plane();
    std::uint64_t calls = 0;
    authority.height = [&](std::int64_t, std::int64_t) {
        ++calls;
        return 0.0f;
    };
    const auto limits = FixtureLimits().authority;
    const FarVolumeRequest held{{3, 6, 7}, {-64, 64}, 91};
    for (const auto& intent : std::vector<FarVolumeCoverageIntent>{
             {{0, 0, 0}, {}},
             {{6, 0, 0}, {}},
             {{1, std::numeric_limits<std::int64_t>::max(), 0}, {}},
             {{1, 0, std::numeric_limits<std::int64_t>::min()}, {}},
             {{1, 0, 0}, FarVolumeCoverageSpan{2, 1}},
             {{1, 0, 0}, FarVolumeCoverageSpan{0, std::numeric_limits<float>::quiet_NaN()}}}) {
        auto output = held;
        EXPECT_FALSE(ResolveFarVolumeCoverage(intent, authority, limits, output));
        EXPECT_EQ(output, held);
    }
    EXPECT_EQ(calls, 0u);
    auto output = held;
    EXPECT_FALSE(ResolveFarVolumeCoverage({{1, 0, 0}, {}}, authority, {16640, 0}, output));
    EXPECT_EQ(calls, 0u);
    authority.numeric = static_cast<FarVolumeNumericProfile>(255);
    EXPECT_FALSE(ResolveFarVolumeCoverage({{1, 0, 0}, {}}, authority, limits, output));
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(output, held);
}

TEST(FarVolumeFacade, HeightFailuresAndFloatWorldGuardsRemainExplicit) {
    for (const float invalid : {std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN(),
                                static_cast<float>(kFarVolumeExactCoordinateLimitMeters - 1024)}) {
        auto authority = Plane();
        authority.height = [invalid](std::int64_t, std::int64_t) {
            return invalid;
        };
        FarVolumeRequest output{{2, 8, 9}, {-32, 32}, 12};
        const auto prior = output;
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(ResolveFarVolumeCoverage(
            {{1, 0, 0}, {}}, authority, FixtureLimits().authority, output, &failure));
        EXPECT_EQ(failure.work.height_samples, 1u);
        EXPECT_EQ(output, prior);
    }
    auto authority = Plane();
    authority.height = [](std::int64_t, std::int64_t) -> std::optional<float> {
        return {};
    };
    FarVolumeRequest output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(ResolveFarVolumeCoverage(
        {{1, 0, 0}, {}}, authority, FixtureLimits().authority, output, &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::SamplerFailure);
}

TEST(FarVolumeFacade, SameAuthorityCompilesAllTiersAndNegativeCoordinates) {
    for (std::size_t tier = 1; tier <= 5; ++tier) {
        SCOPED_TRACE(tier);
        FarVolumeCompilation result;
        ASSERT_TRUE(CompileFarVolume({{tier, -1, -2}, {}}, Plane(), FixtureLimits(), result));
        EXPECT_EQ(result.tile.request, result.mesh.request);
        EXPECT_EQ(result.mesh.vertices.size(), 129u * 129u);
        EXPECT_EQ(result.mesh.indices.size(), 128u * 128u * 6u);
        ASSERT_TRUE(result.mesh.bounds);
        EXPECT_EQ(result.mesh.origin_meters.y + result.mesh.bounds->min[1], -.25);
        EXPECT_GT(result.work.height_samples, 129u * 129u) << "actual normal halo must be sampled";
        EXPECT_LE(result.work.height_samples, 131u * 131u);
        EXPECT_EQ(result.work.density_samples,
                  result.tile.samples_evaluated + result.mesh.samples_evaluated);
        EXPECT_TRUE(result.legacy_checksum);
        for (const auto& vertex : result.mesh.vertices) {
            EXPECT_EQ(vertex.normal, (FarVolumeVector{0, 1, 0}));
            EXPECT_EQ(vertex.material, 7);
        }
    }
}

TEST(FarVolumeFacade, NumericProfilesStayExplicitAndUseTheOriginalCodec) {
    for (int encoded = -32767; encoded <= 32767; ++encoded) {
        const auto value = DequantizeFarLodSdf(static_cast<std::int16_t>(encoded));
        EXPECT_EQ(QuantizeFarLodSdf(value), encoded);
    }
    EXPECT_EQ(QuantizeFarLodSdf(std::numeric_limits<float>::denorm_min()), 1);
    EXPECT_EQ(QuantizeFarLodSdf(-std::numeric_limits<float>::denorm_min()), -1);
    EXPECT_EQ(QuantizeFarLodSdf(std::numeric_limits<float>::quiet_NaN()), kFarLodSdfInvalid);
    EXPECT_EQ(QuantizeFarLodSdf(std::numeric_limits<float>::max()), 32767);
    EXPECT_EQ(QuantizeFarLodSdf(-std::numeric_limits<float>::max()), -32767);
    auto authority = Plane(FarVolumeNumericProfile::RawFloat);
    authority.density = [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{static_cast<float>(p.y) + .001f, 255};
    };
    FarVolumeCompilation raw, quantized;
    ASSERT_TRUE(CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), raw));
    authority.numeric = FarVolumeNumericProfile::LegacySigned16;
    ASSERT_TRUE(CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), quantized));
    EXPECT_FALSE(raw.legacy_checksum);
    EXPECT_TRUE(quantized.legacy_checksum);
    ASSERT_TRUE(raw.mesh.bounds);
    ASSERT_TRUE(quantized.mesh.bounds);
    EXPECT_GT(std::abs(raw.mesh.bounds->min[1] - quantized.mesh.bounds->min[1]), .002);
    std::uint32_t checksum = 123;
    EXPECT_FALSE(FarVolumeLegacyChecksum(raw.tile, raw.caves, checksum));
    EXPECT_EQ(checksum, 123u);
}

TEST(FarVolumeFacade, CallbackAndHaloBudgetsRefuseWithoutPartialPublication) {
    FarVolumeCompilation prior;
    prior.intent.key = {3, -7, 9};
    const auto held = prior;
    auto limits = FixtureLimits();
    limits.authority.max_density_samples = 0;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, Plane(), limits, prior, &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::DensityLimit);
    EXPECT_EQ(failure.work.density_samples, 0u);
    EXPECT_EQ(prior, held);
    limits = FixtureLimits();
    limits.authority.max_height_samples = 129 * 129;
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, Plane(), limits, prior, &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::HeightLimit);
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::SamplerFailure);
    EXPECT_EQ(failure.work.height_samples, 129u * 129u);
    EXPECT_EQ(prior, held);
    limits = FixtureLimits();
    limits.mesh.max_vertices = 0;
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, Plane(), limits, prior, &failure));
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::VertexLimit);
    EXPECT_EQ(prior, held);
}

TEST(FarVolumeFacade, ChangedAuthorityAndMissingHaloCannotBecomeReadyEmpty) {
    auto authority = Plane();
    std::uint64_t samples = 0;
    constexpr std::uint64_t generation_samples = 17u * 1024u * 125u;
    authority.density = [&](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{
            static_cast<float>(p.y) + (++samples > generation_samples ? 1.0f : .25f), 7};
    };
    FarVolumeCompilation output;
    output.intent.key = {5, 7, 8};
    const auto prior = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), output, &failure));
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::SampleMismatch);
    EXPECT_EQ(output, prior);
    authority = Plane();
    authority.height = [](std::int64_t x, std::int64_t) -> std::optional<float> {
        return x < 0 ? std::nullopt : std::optional<float>(0);
    };
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), output, &failure));
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::SamplerFailure);
    EXPECT_EQ(output, prior);
}

TEST(FarVolumeFacade, AuthorityExceptionsAndNonFiniteDensityAreTransactional) {
    struct ThrowOnCopy {
        ThrowOnCopy() = default;
        ThrowOnCopy(const ThrowOnCopy&) {
            throw std::runtime_error("authority copy failed");
        }
        ThrowOnCopy(ThrowOnCopy&&) = default;
        std::optional<float> operator()(std::int64_t, std::int64_t) const {
            return 0;
        }
    };
    auto authority = Plane();
    authority.height = ThrowOnCopy{};
    FarVolumeCompilation output;
    output.intent.key = {5, 7, 9};
    const auto held = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), output, &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::SamplerFailure);
    EXPECT_EQ(failure.work.height_samples, 0u);
    EXPECT_EQ(output, held);
    FarVolumeRequest request{{4, 7, 9}, {-128, 128}, 97};
    const auto held_request = request;
    EXPECT_FALSE(ResolveFarVolumeCoverage(
        {{1, 0, 0}, {}}, authority, FixtureLimits().authority, request, &failure));
    EXPECT_EQ(request, held_request);
    for (unsigned kind = 0; kind < 4; ++kind) {
        authority = Plane();
        authority.density = [kind](const FarVolumePosition&,
                                   float,
                                   std::uint32_t,
                                   FarCaveMode) -> std::optional<FarVolumeSample> {
            if (kind == 0)
                throw std::runtime_error("density failed");
            if (kind == 1)
                return {};
            return FarVolumeSample{kind == 2 ? std::numeric_limits<float>::infinity()
                                             : std::numeric_limits<float>::quiet_NaN(),
                                   3};
        };
        EXPECT_FALSE(
            CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), output, &failure));
        EXPECT_EQ(failure.code,
                  kind < 2 ? FarVolumeFacadeError::SamplerFailure
                           : FarVolumeFacadeError::NonFiniteSample);
        EXPECT_EQ(failure.work.density_samples, 1u);
        EXPECT_EQ(output, held);
    }
    for (const bool fail_height : {false, true}) {
        authority = Plane();
        if (fail_height) {
            authority.height = [](std::int64_t, std::int64_t) -> std::optional<float> {
                throw std::bad_alloc();
            };
        } else {
            authority.density = [](const FarVolumePosition&,
                                   float,
                                   std::uint32_t,
                                   FarCaveMode) -> std::optional<FarVolumeSample> {
                throw std::bad_alloc();
            };
        }
        EXPECT_FALSE(
            CompileFarVolume({{1, 0, 0}, {}}, authority, FixtureLimits(), output, &failure));
        EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
        EXPECT_EQ(failure.work.height_samples, fail_height ? 1u : 129u * 129u);
        EXPECT_EQ(failure.work.density_samples, fail_height ? 0u : 1u);
        EXPECT_EQ(output, held);
    }
}

TEST(FarVolumeFacade, EmptyAirAndSolidKeepDistinctMetadataAndLegacyChecksumScope) {
    std::optional<std::uint32_t> checksum;
    for (const float density : {1.0f, -1.0f}) {
        auto authority = Plane();
        authority.density = [density](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{density, 7};
        };
        FarVolumeCompilation output;
        ASSERT_TRUE(CompileFarVolume({{5, 0, 0}, {}}, authority, FixtureLimits(), output));
        EXPECT_EQ(output.tile.content,
                  density < 0 ? FarVolumeContent::Solid : FarVolumeContent::Air);
        EXPECT_EQ(output.mesh.content, output.tile.content);
        EXPECT_TRUE(output.mesh.vertices.empty());
        EXPECT_FALSE(output.mesh.bounds);
        ASSERT_TRUE(output.legacy_checksum);
        if (checksum) {
            EXPECT_EQ(output.legacy_checksum, checksum)
                << "the old sparse checksum does not encode absent homogeneous density";
        }
        checksum = output.legacy_checksum;
    }
}

TEST(FarVolumeFacade, LegacyChecksumRejectsMalformedOrInconsistentSparseStreams) {
    FarVolumeCompilation result;
    ASSERT_TRUE(CompileFarVolume({{5, -1, -1}, {}}, Plane(), FixtureLimits(), result));
    const auto good = result.tile;
    std::uint32_t checksum = 123;
    auto corrupt = good;
    std::reverse(corrupt.bricks.begin(), corrupt.bricks.end());
    EXPECT_FALSE(FarVolumeLegacyChecksum(corrupt, result.caves, checksum));
    corrupt = good;
    ++corrupt.samples_evaluated;
    EXPECT_FALSE(FarVolumeLegacyChecksum(corrupt, result.caves, checksum));
    corrupt = good;
    // Shared X face: change a still-finite/encoded material, not its sign.
    corrupt.bricks[0].samples[4].material ^= 1;
    EXPECT_FALSE(FarVolumeLegacyChecksum(corrupt, result.caves, checksum));
    EXPECT_EQ(checksum, 123u);
}

void MixReference(Luminumbra::Core::Crc32Accumulator& crc, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) {
        const auto byte = static_cast<std::uint8_t>(value >> (8 * i));
        crc.Update(&byte, 1);
    }
}

TEST(FarVolumeFacade,
     MatchesPinnedFoundationDiscoveryEncodedLatticeMaterialsSparseStreamsAndChecksums) {
    std::ifstream input(std::string(LUMINUMBRA_SOURCE_ROOT) +
                        "/test/fixtures/far_volume/foundation-reference.json");
    ASSERT_TRUE(input.good());
    const auto reference = nlohmann::json::parse(input);
    ASSERT_EQ(reference.at("foundation_commit"), "70b899e7b2ed60407a9d42da2089e99192d95b54");
    ASSERT_EQ(reference.at("runtime_reference_commit"), "6a13b70e95f0c4a152f09e5df3422931c10dccfa");
    ASSERT_EQ(reference.at("cases").size(), 10u);
    namespace Fixture = FarVolumeFoundationFixture;
    for (const auto& original : reference.at("cases")) {
        const auto tier = original.at("tier").get<std::size_t>();
        const auto caves = static_cast<FarCaveMode>(original.at("caves").get<unsigned>());
        const auto field = original.at("field").get<unsigned>();
        SCOPED_TRACE(original.dump());
        const auto d = *FarTierAt(tier);
        const FarVolumeTileKey key{tier, original.at("tile_x"), original.at("tile_z")};
        const auto origin_x = key.x * d.tile_edge_meters;
        const auto origin_z = key.z * d.tile_edge_meters;
        const auto first_y = original.at("first_brick_y").get<std::int64_t>() * d.brick_edge_meters;
        const auto last_y = original.at("last_brick_y").get<std::int64_t>() * d.brick_edge_meters;
        const auto ny = static_cast<std::size_t>((last_y - first_y) / d.sample_spacing_meters + 1);
        struct Recorded {
            std::int16_t density = 0;
            std::uint8_t material = 0;
            bool seen = false;
        };
        std::vector<Recorded> lattice(129 * 129 * ny);
        std::uint64_t height_calls = 0;
        Luminumbra::Core::Crc32Accumulator heights;
        FarVolumeAuthority authority{
            891,
            caves,
            FarVolumeNumericProfile::LegacySigned16,
            [&](std::int64_t x, std::int64_t z) {
                const auto h = Fixture::Height(static_cast<float>(x), static_cast<float>(z));
                if (++height_calls <= 129u * 129u)
                    MixReference(heights, std::bit_cast<std::uint32_t>(h), 4);
                return h;
            },
            [&](const FarVolumePosition& p,
                float h,
                std::uint32_t spacing,
                FarCaveMode actual_caves) {
                EXPECT_EQ(actual_caves, caves);
                EXPECT_EQ(spacing, d.sample_spacing_meters);
                const auto density = Fixture::Density(static_cast<float>(p.y), h, field);
                const auto material = Fixture::Material(p.x, p.y, p.z, spacing);
                // Observe every distinct generator lattice coordinate. Halo calls
                // outside that interval are deliberately excluded from this old
                // foundation receipt; separate tests require them for normals.
                if (p.x >= origin_x && p.x <= origin_x + d.tile_edge_meters && p.z >= origin_z &&
                    p.z <= origin_z + d.tile_edge_meters && p.y >= first_y && p.y <= last_y) {
                    const auto x = static_cast<std::size_t>((p.x - origin_x) / spacing);
                    const auto y = static_cast<std::size_t>((p.y - first_y) / spacing);
                    const auto z = static_cast<std::size_t>((p.z - origin_z) / spacing);
                    auto& record = lattice[x + 129 * (y + ny * z)];
                    const auto q = QuantizeFarLodSdf(density);
                    if (record.seen) {
                        EXPECT_EQ(record.density, q);
                        EXPECT_EQ(record.material, material);
                    }
                    record = {q, material, true};
                }
                return FarVolumeSample{density, material};
            }};
        FarVolumeCoverageIntent intent{key, {}};
        if (!original.at("extra_span").is_null())
            intent.extra_span =
                FarVolumeCoverageSpan{original.at("extra_span")[0], original.at("extra_span")[1]};
        FarVolumeCompilation result;
        FarVolumeFacadeFailure failure;
        ASSERT_TRUE(CompileFarVolume(intent, authority, FixtureLimits(), result, &failure))
            << "facade=" << static_cast<int>(failure.code)
            << " mesh=" << static_cast<int>(failure.mesh);
        EXPECT_EQ(result.tile.request.span, (FarVolumeSpan{first_y, last_y}));
        EXPECT_EQ(result.tile.candidates_visited, original.at("candidates").get<std::uint64_t>());
        EXPECT_EQ(result.tile.bricks.size(), original.at("sparse_bricks").get<std::size_t>());
        ASSERT_TRUE(result.legacy_checksum);
        EXPECT_EQ(*result.legacy_checksum, original.at("tile_crc32").get<std::uint32_t>());
        EXPECT_EQ(heights.Value(), original.at("height_crc32").get<std::uint32_t>());
        EXPECT_EQ(lattice.size(), original.at("density_count").get<std::size_t>());
        Luminumbra::Core::Crc32Accumulator encoded, materials;
        for (const auto& record : lattice) {
            ASSERT_TRUE(record.seen) << "discovery must scan absent homogeneous bricks too";
            MixReference(encoded, static_cast<std::uint16_t>(record.density), 2);
            MixReference(encoded, record.material, 1);
            MixReference(materials, record.material, 1);
        }
        EXPECT_EQ(encoded.Value(), original.at("encoded_lattice_crc32").get<std::uint32_t>());
        EXPECT_EQ(materials.Value(), original.at("material_lattice_crc32").get<std::uint32_t>());
    }
}

TEST(FarVolumeFacade, OriginalFiniteBoxFilterRetainsTapOrderAndRefusals) {
    for (const std::uint32_t spacing : {16u, 32u, 64u}) {
        std::vector<glm::vec3> taps;
        const auto value =
            BoxFilterFarVolumeCarve({-256, 8, 512}, spacing, [&](const glm::vec3& p) {
                taps.push_back(p);
                return static_cast<float>(taps.size());
            });
        EXPECT_EQ(value, 4.5f);
        ASSERT_EQ(taps.size(), 8u);
        const float half_tap = spacing * .25f;
        EXPECT_EQ(taps.front(), (glm::vec3{-256 - half_tap, 8 - half_tap, 512 - half_tap}));
        EXPECT_EQ(taps.back(), (glm::vec3{-256 + half_tap, 8 + half_tap, 512 + half_tap}));
    }
    const auto constant = [](const glm::vec3&) {
        return 1.0f;
    };
    EXPECT_THROW(BoxFilterFarVolumeCarve({}, 8, constant), std::invalid_argument);
    EXPECT_THROW(
        BoxFilterFarVolumeCarve(
            {static_cast<float>(kFarVolumeExactCoordinateLimitMeters), 0, 0}, 16, constant),
        std::invalid_argument);
    EXPECT_THROW(BoxFilterFarVolumeCarve({}, 16, [](const glm::vec3&) { return -1.0f; }),
                 std::invalid_argument);
}
} // namespace
