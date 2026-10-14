#include "luminumbra_client/rendering/FarWorldCaveSelection.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;

Systems::TerrainGenParams CaveParams() {
    Systems::TerrainGenParams params;
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.continental_spline = {{-1, 0}, {1, 0}};
    params.caves_enabled = true;
    params.surface_breaks_enabled = true;
    params.surface_break_density = 1;
    return params;
}

void ExpectSameBits(float a, float b) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(a), std::bit_cast<std::uint32_t>(b));
}

using SampleKey = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
using BorderSamples = std::map<SampleKey, FarVolumeSample>;

// Curved immutable qualification field: varies in all axes, has surfaces on
// both sides of Y=0, and gives the two policies different density/material bits.
// This oracle is independent of the facade's tile origins and border indexing.
double BorderHeight(double x, double z, double edge) {
    const auto dx = x + 1.5 * edge, dz = z + .5 * edge;
    return (dx * dx + 2 * dz * dz) / (128 * edge) - edge / 256;
}
FarVolumeSample
BorderField(const FarVolumePosition& p, const FarTierDimensions& tier, FarCaveMode policy) {
    // Avoid exact-zero lattice corners, where distinct incident edges may
    // legitimately duplicate a mesh vertex differently on either side.
    const auto shift = policy == FarCaveMode::BandLimited
                           ? .123456789
                           : .123456789 + tier.sample_spacing_meters / 8.0;
    const auto material = static_cast<std::uint8_t>(
        (static_cast<std::uint64_t>(p.x) + 3 * static_cast<std::uint64_t>(p.y) +
         7 * static_cast<std::uint64_t>(p.z) + static_cast<std::uint8_t>(policy)) &
        255u);
    return {static_cast<float>(p.y - BorderHeight(p.x, p.z, tier.tile_edge_meters) + shift),
            material};
}

void ExpectSharedFace(const BorderSamples& a,
                      const BorderSamples& b,
                      const FarVolumeSpan& span_a,
                      const FarVolumeSpan& span_b,
                      const FarTierDimensions& tier,
                      int axis,
                      std::int64_t plane,
                      std::int64_t tangent_min) {
    const auto min_y = std::max(span_a.min_y, span_b.min_y);
    const auto max_y = std::min(span_a.max_y, span_b.max_y);
    ASSERT_LT(min_y, max_y);
    // Full face, including homogeneous samples which sparse bricks omit.
    for (auto y = min_y; y <= max_y; y += tier.sample_spacing_meters) {
        for (auto t = tangent_min; t <= tangent_min + tier.tile_edge_meters;
             t += tier.sample_spacing_meters) {
            const SampleKey key = axis == 0 ? SampleKey{plane, y, t} : SampleKey{t, y, plane};
            const auto ai = a.find(key), bi = b.find(key);
            ASSERT_NE(ai, a.end());
            ASSERT_NE(bi, b.end());
            ExpectSameBits(ai->second.density, bi->second.density);
            EXPECT_EQ(ai->second.material, bi->second.material);
        }
    }
}

void ExpectSharedMeshFace(const FarVolumeMesh& a,
                          const FarVolumeMesh& b,
                          const FarTierDimensions& tier,
                          int axis,
                          double plane) {
    const auto vertices = [axis, plane, &tier](const FarVolumeMesh& mesh) {
        std::vector<FarVolumeVertex> face;
        for (auto vertex : mesh.vertices) {
            vertex.position[0] += mesh.origin_meters.x;
            vertex.position[1] += mesh.origin_meters.y;
            vertex.position[2] += mesh.origin_meters.z;
            if (vertex.position[axis] != plane)
                continue;
            // Central differences of this quadratic field equal its analytic
            // gradient. Wrong halo coordinates must also fail this oracle.
            const double edge = tier.tile_edge_meters;
            FarVolumeVector normal{-(vertex.position[0] + 1.5 * edge) / (64 * edge),
                                   1,
                                   -(vertex.position[2] + .5 * edge) / (32 * edge)};
            const auto length = std::sqrt(normal[0] * normal[0] + 1 + normal[2] * normal[2]);
            for (std::size_t i = 0; i < 3; ++i)
                EXPECT_NEAR(vertex.normal[i], normal[i] / length, 1e-6);
            face.push_back(vertex);
        }
        std::sort(face.begin(), face.end(), [](const auto& x, const auto& y) {
            return x.position < y.position;
        });
        return face;
    };
    const auto af = vertices(a), bf = vertices(b);
    ASSERT_FALSE(af.empty());
    ASSERT_EQ(af.size(), bf.size());
    for (std::size_t i = 0; i < af.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            EXPECT_NEAR(af[i].position[j], bf[i].position[j], 1e-9);
            EXPECT_NEAR(af[i].normal[j], bf[i].normal[j], 1e-9);
        }
        EXPECT_EQ(af[i].material, bf[i].material);
    }
}

void ExpectSharedHaloFace(const BorderSamples& a,
                          const BorderSamples& b,
                          int axis,
                          std::int64_t plane) {
    std::size_t shared = 0;
    for (const auto& [key, sample] : a) {
        if ((axis == 0 ? std::get<0>(key) : std::get<2>(key)) != plane)
            continue;
        const auto found = b.find(key);
        if (found != b.end()) {
            ++shared;
            ExpectSameBits(sample.density, found->second.density);
            EXPECT_EQ(sample.material, found->second.material);
        }
    }
    EXPECT_GT(shared, 0u); // Exterior normal halo in one build, interior in its neighbour.
}

void ExpectSharedVerticalBrickFace(const FarVolumeTile& tile,
                                   const FarTierDimensions& tier,
                                   FarCaveMode policy) {
    BorderSamples below, above;
    for (const auto& brick : tile.bricks) {
        if (brick.origin_meters.y != -static_cast<std::int64_t>(tier.brick_edge_meters) &&
            brick.origin_meters.y != 0)
            continue;
        const auto sy = brick.origin_meters.y == 0 ? 0u : kFarVolumeBrickSide - 1;
        auto& face = brick.origin_meters.y == 0 ? above : below;
        for (std::size_t z = 0; z < kFarVolumeBrickSide; ++z) {
            for (std::size_t x = 0; x < kFarVolumeBrickSide; ++x) {
                const FarVolumePosition p{
                    brick.origin_meters.x +
                        static_cast<std::int64_t>(x * tier.sample_spacing_meters),
                    0,
                    brick.origin_meters.z +
                        static_cast<std::int64_t>(z * tier.sample_spacing_meters)};
                const auto sample =
                    brick.samples[x + kFarVolumeBrickSide * (sy + kFarVolumeBrickSide * z)];
                const auto expected = BorderField(p, tier, policy);
                ExpectSameBits(sample.density, expected.density);
                EXPECT_EQ(sample.material, expected.material);
                face[{p.x, p.y, p.z}] = sample;
            }
        }
    }
    std::size_t shared = 0;
    for (const auto& [key, sample] : below) {
        const auto found = above.find(key);
        if (found != above.end()) {
            ++shared;
            ExpectSameBits(sample.density, found->second.density);
            EXPECT_EQ(sample.material, found->second.material);
        }
    }
    EXPECT_GT(shared, 0u); // Independently generated lower/upper brick copies.
}
} // namespace

TEST(FarWorldCaveSelection, DefaultAndExactCodesReuseCanonicalEnum) {
    using luminumbra::core::SysParam;
    using luminumbra::core::SystemConfig;
    EXPECT_EQ(kDefaultFarWorldCavePolicy, FarCaveMode::BandLimited);
    EXPECT_EQ(ResolveFarWorldCaveField(SystemConfig::Defaults().param(
                  SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)),
              FarCaveMode::BandLimited);
    EXPECT_EQ(ResolveFarWorldCaveField(0), FarCaveMode::BandLimited);
    EXPECT_EQ(ResolveFarWorldCaveField(-0.0), FarCaveMode::BandLimited);
    EXPECT_EQ(ResolveFarWorldCaveField(1), FarCaveMode::BoxFiltered);
    // The default renderer requests Ladder; without authority it has no effective cave policy.
    const FarWorldRenderer renderer;
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).requested, FarWorldMode::Ladder);
    const auto policy = renderer.CavePolicy(FarWorldPurpose::World);
    EXPECT_EQ(policy.requested, FarCaveMode::BandLimited);
    EXPECT_FALSE(policy.bound);
    EXPECT_FALSE(policy.effective);
}

TEST(FarWorldCaveSelection, NonfiniteFractionalAndUnsupportedCodesRefuseWithoutRounding) {
    for (const double code : {-1.0,
                              2.0,
                              255.0,
                              .5,
                              1.5,
                              std::nextafter(0.0, 1.0),
                              std::nextafter(1.0, 0.0),
                              std::nextafter(1.0, 2.0),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        SCOPED_TRACE(code);
        const auto requested = ResolveFarWorldCaveField(code);
        EXPECT_FALSE(requested);
        FarWorldRenderer renderer(FarWorldMode::Ladder);
        renderer.SetRequestedCavePolicy(requested);
        const auto selection = renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::InvalidCavePolicy);
        EXPECT_FALSE(renderer.CavePolicy(selection).requested);
        JobSystem jobs;
        EXPECT_EQ(renderer.BindFixture(jobs, {}, 1, 1, FarWorldDevelopmentLimits()),
                  FarWorldRefusal::InvalidCavePolicy);
    }
}

TEST(FarWorldCaveSelection, LegacyPreviewAndShaderRefusalHaveNoEffectiveCavePolicy) {
    for (const auto requested : {std::optional(FarCaveMode::BandLimited),
                                 std::optional(FarCaveMode::BoxFiltered),
                                 std::optional<FarCaveMode>{}}) {
        FarWorldRenderer legacy(FarWorldMode::Legacy);
        legacy.SetRequestedCavePolicy(requested);
        const auto selected = legacy.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selected.effective, FarWorldMode::Legacy);
        EXPECT_EQ(selected.refusal, FarWorldRefusal::None);
        EXPECT_TRUE(selected.advance);
        EXPECT_TRUE(selected.submit);
        EXPECT_EQ(legacy.CavePolicy(selected).requested, requested);
        EXPECT_FALSE(legacy.CavePolicy(selected).effective);
    }
    // Final pass selection can refuse an otherwise valid bound Ladder request.
    for (const auto purpose :
         {FarWorldPurpose::World, FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
        const auto selection = SelectFarWorld(
            FarWorldMode::Ladder, purpose, FarWorldRefusal::ShaderProgramUnavailable);
        const auto policy =
            SelectFarWorldCavePolicy(FarCaveMode::BoxFiltered, FarCaveMode::BoxFiltered, selection);
        EXPECT_EQ(policy.requested, FarCaveMode::BoxFiltered);
        EXPECT_EQ(policy.bound, FarCaveMode::BoxFiltered);
        EXPECT_FALSE(policy.effective);
    }
}

TEST(FarWorldCaveSelection, V1V2BothPoliciesAreBitIdenticalToLiveRouter) {
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, CaveParams(), 1337);
    const auto scope = world.acquire_worldgen_sample_scope();
    for (const std::uint32_t spacing : {4u, 8u}) {
        for (const float x : {-8192.0f, -512.0f, 0.0f, 4096.0f, 16320.0f}) {
            for (const float y : {-256.0f, -64.0f, -18.0f, 0.0f, 32.0f}) {
                const Vec3 p{x, y, -x};
                const auto height = world.GetTerrainHeightAt(p.x, p.z);
                const auto live = world.get_density_at_from_precalculated(p, height);
                for (const auto code : {0.0, 1.0}) {
                    const auto policy = ResolveFarWorldCaveField(code);
                    ASSERT_TRUE(policy);
                    if (policy) {
                        ExpectSameBits(world.SamplePristineFarDensity(p, height, spacing, *policy),
                                       live);
                    }
                }
            }
        }
    }
}

TEST(FarWorldCaveSelection, ProductionFacadeAdjacentTilesShareBordersHalosAndVerticalBrickSamples) {
    for (std::size_t index = 0; index < kFarTierCount; ++index) {
        const auto& tier = kFarTierTable[index];
        const auto edge = static_cast<std::int64_t>(tier.tile_edge_meters);
        const auto spacing = static_cast<std::int64_t>(tier.sample_spacing_meters);
        for (const auto policy : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            SCOPED_TRACE(index + 1);
            SCOPED_TRACE(static_cast<unsigned>(policy));
            const std::array<FarVolumeTileKey, 3> keys{
                FarVolumeTileKey{index + 1, -2, -1}, {index + 1, -1, -1}, {index + 1, -2, 0}};
            FarVolumeMemoryPool pool(256 * 1024 * 1024);
            std::array<FarVolumeMemoryLease, 3> retained{
                FarVolumeMemoryLease(pool), FarVolumeMemoryLease(pool), FarVolumeMemoryLease(pool)};
            std::array<FarVolumeCompilation, 3> compiled;
            std::array<BorderSamples, 3> samples;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const auto origin_x = keys[i].x * edge, origin_z = keys[i].z * edge;
                bool saw_x_halo = false, saw_z_halo = false;
                FarVolumeAuthority authority{
                    42u + static_cast<std::uint8_t>(policy),
                    policy,
                    FarVolumeNumericProfile::RawFloat,
                    [&tier](std::int64_t x, std::int64_t z) {
                        return static_cast<float>(BorderHeight(x, z, tier.tile_edge_meters));
                    },
                    [&](const FarVolumePosition& p,
                        float,
                        std::uint32_t actual_spacing,
                        FarCaveMode actual_policy) -> std::optional<FarVolumeSample> {
                        if (actual_policy != policy ||
                            actual_spacing != tier.sample_spacing_meters) {
                            ADD_FAILURE() << "facade lost the selected policy/spacing";
                            return std::nullopt;
                        }
                        saw_x_halo = saw_x_halo || p.x < origin_x || p.x > origin_x + edge;
                        saw_z_halo = saw_z_halo || p.z < origin_z || p.z > origin_z + edge;
                        const auto sample = BorderField(p, tier, actual_policy);
                        if (p.x == -edge || p.x == -edge - spacing || p.x == -edge + spacing ||
                            p.z == 0 || p.z == -spacing || p.z == spacing) {
                            const auto [at, inserted] =
                                samples[i].emplace(SampleKey{p.x, p.y, p.z}, sample);
                            if (!inserted) {
                                ExpectSameBits(at->second.density, sample.density);
                                EXPECT_EQ(at->second.material, sample.material);
                            }
                        }
                        return sample;
                    }};
                FarVolumeFacadeFailure failure;
                // Same charged discovery/generation/halo/meshing facade used
                // by the renderer's queue, with an independently owned result.
                ASSERT_TRUE(CompileFarVolumeCharged(
                    {keys[i], {}},
                    authority,
                    FarWorldDevelopmentLimits().residency.queues[index].compilation,
                    {&pool, &retained[i], nullptr},
                    compiled[i],
                    &failure))
                    << "facade=" << static_cast<int>(failure.code);
                EXPECT_EQ(compiled[i].caves, policy);
                EXPECT_EQ(compiled[i].tile.min_meters.x, origin_x);
                EXPECT_EQ(compiled[i].tile.min_meters.z, origin_z);
                EXPECT_EQ(compiled[i].tile.max_meters.x, origin_x + edge);
                EXPECT_EQ(compiled[i].tile.max_meters.z, origin_z + edge);
                EXPECT_TRUE(saw_x_halo);
                EXPECT_TRUE(saw_z_halo);
                for (const auto& [key, sample] : samples[i]) {
                    const auto& [x, y, z] = key;
                    const auto expected = BorderField({x, y, z}, tier, policy);
                    ExpectSameBits(sample.density, expected.density);
                    EXPECT_EQ(sample.material, expected.material);
                }
            }
            // Adjacent X and Z tiles include negative origins and the zero seam.
            // Compare the actual independently generated faces and normal halos.
            ExpectSharedFace(samples[0],
                             samples[1],
                             compiled[0].tile.request.span,
                             compiled[1].tile.request.span,
                             tier,
                             0,
                             -edge,
                             -edge);
            ExpectSharedFace(samples[0],
                             samples[2],
                             compiled[0].tile.request.span,
                             compiled[2].tile.request.span,
                             tier,
                             2,
                             0,
                             -2 * edge);
            for (const auto plane : {-edge - spacing, -edge + spacing})
                ExpectSharedHaloFace(samples[0], samples[1], 0, plane);
            for (const auto plane : {-spacing, spacing})
                ExpectSharedHaloFace(samples[0], samples[2], 2, plane);
            ExpectSharedMeshFace(compiled[0].mesh, compiled[1].mesh, tier, 0, -edge);
            ExpectSharedMeshFace(compiled[0].mesh, compiled[2].mesh, tier, 2, 0);
            ExpectSharedVerticalBrickFace(compiled[0].tile, tier, policy);
        }
    }
}

TEST(FarWorldCaveSelection, AnalyticOpeningSilhouettesRemainUnfilteredThroughOuterRadius) {
    auto params = CaveParams();
    // Prevent the noise router lifting the surface cap. At this shallow slice
    // all eight taps (including 64 m spacing) remain above the 18 m cap, leaving
    // the signed analytic mouth as an independent silhouette oracle.
    params.cave_threshold = 1;
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto scope = world.acquire_worldgen_sample_scope();
    for (const float station : {-15872.0f, -8192.0f, 4096.0f, 8192.0f, 15872.0f}) {
        const auto mouth = world.FindLargestSurfaceBreak(station, 0, 256);
        ASSERT_TRUE(mouth.found);
        ASSERT_GT(mouth.depth, .5f);
        bool saw_air = false, saw_solid = false;
        for (int i = 0; i <= 16; ++i) {
            const Vec3 p{mouth.x + mouth.radius * static_cast<float>(i) / 8, -.5f, mouth.z};
            const auto height = world.GetTerrainHeightAt(p.x, p.z);
            const auto analytic = world.AnalyticSurfaceOpeningDensity(p, height);
            saw_air = saw_air || analytic > 0;
            saw_solid = saw_solid || analytic < 0;
            for (const auto spacing : {16u, 32u, 64u}) {
                for (const auto code : {0.0, 1.0}) {
                    const auto policy = ResolveFarWorldCaveField(code);
                    ASSERT_TRUE(policy);
                    if (policy) {
                        ExpectSameBits(world.SamplePristineFarDensity(p, height, spacing, *policy),
                                       analytic);
                    }
                }
            }
        }
        EXPECT_TRUE(saw_air);
        EXPECT_TRUE(saw_solid);
    }
}

TEST(FarWorldCaveSelection, OldCompletedBuildRefusesChangedContentOrCaveIdentity) {
    for (int changed = -1; changed < 3; ++changed) {
        JobSystem jobs;
        jobs.startup(1);
        FarVolumeMemoryPool pool(64 * 1024 * 1024);
        auto limits = FarWorldDevelopmentLimits().residency.queues.back();
        limits.descriptor_slots = limits.result_slots = 1;
        FarVolumeBuildQueue queue(jobs, pool, limits);
        FarVolumeQueueRequest request;
        request.identity = {1, 42, 0, {5, 0, 0}, 1, 1};
        request.intent = {{5, 0, 0}, {}};
        request.authority = {42,
                             FarCaveMode::BandLimited,
                             FarVolumeNumericProfile::RawFloat,
                             [](std::int64_t, std::int64_t) { return 0.0f; },
                             [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
                                 return FarVolumeSample{1, 7};
                             }};
        request.water = std::make_shared<const FarWorldConstantWaterObservation>(42, false, 0);
        request.water_bound = FarWorldWaterBound{true, 0, 0};
        ASSERT_EQ(queue.Submit(request), FarVolumeSubmitStatus::Queued);
        ASSERT_EQ(queue.Pump(1), 1u);
        jobs.shutdown(); // Old completion exists; consumption still belongs to owner.
        queue.Pump(0);
        ASSERT_EQ(queue.Stats().ready, 1u);
        if (changed == -1) {
            const auto matching = queue.TakeCompleted(request.identity, request.intent);
            ASSERT_TRUE(matching);
            if (matching) {
                EXPECT_EQ(matching->status(), FarVolumeBuildStatus::ReadyEmpty);
                EXPECT_EQ(matching->identity(), request.identity);
            }
            continue;
        }
        auto expected = request.identity;
        if (changed != 1)
            ++expected.content_hash;
        if (changed != 0)
            expected.cave_policy = static_cast<std::uint8_t>(FarCaveMode::BoxFiltered);
        EXPECT_FALSE(queue.TakeCompleted(expected, request.intent));
        EXPECT_EQ(queue.Stats().stale, 1u);
        EXPECT_EQ(queue.Stats().ready, 0u);
        EXPECT_FALSE(queue.TakeCompleted(request.identity, request.intent));
        EXPECT_EQ(queue.Stats().retained_bytes, 0u);
    }
}
