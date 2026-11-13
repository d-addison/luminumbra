#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarFrozenObservation.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/FarVolumeRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

namespace {
using namespace Luminumbra::World;

FarVolumeCompilationLimits PristineLimits() {
    // Explicit test limits, not approved runtime residency caps.
    return {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}};
}
FarVolumeCoverageIntent PristineIntent() {
    return {{1, 0, 0}, {}};
}
FarVolumeRequest PristineHeld() {
    return {{3, 6, 7}, {-64, 64}, 91};
}
Luminumbra::Systems::TerrainGenParams PristinePlaneParams() {
    Luminumbra::Systems::TerrainGenParams params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.peaks_amplitude = 0.0f;
    params.continental_spline = {{-1.0f, 0.0f}, {1.0f, 0.0f}};
    params.height_offset = 0.25f;
    params.caves_enabled = false;
    return params;
}
FarVolumeCoverageIntent PristinePlaneIntent() {
    // V5 keeps the 256 m interior band to two layers of 256 m bricks.
    return {{5, 0, 0}, {}};
}
FarVolumeCompilationLimits PristinePlaneLimits() {
    // One mixed brick per 32x32 column, 128x128 surface cells, two triangles
    // per surface cell and one vertex per 129x129 horizontal lattice point.
    return {{131 * 131, 1'000'000},
            {2 * 32 * 32, 32 * 32},
            {32 * 32 * 64, 300'000, 129 * 129, 128 * 128 * 6}};
}
} // namespace

TEST(FarVolumeWorld, SessionIdentityCoversResolvedInputsAndRefusesStaleCapture) {
    auto dependencies = std::make_shared<WorldgenDependencySnapshot>();
    dependencies->manifest = {{"table", "content-a", "accepted"}};
    auto params = PristinePlaneParams();
    params.worldgen_dependencies = dependencies;
    Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 17);
    const auto identity = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4);
    ASSERT_NE(identity, 0u);
    auto equal_inputs = std::make_shared<WorldgenDependencySnapshot>(*dependencies);
    params.worldgen_dependencies = equal_inputs;
    world.set_params(params);
    EXPECT_EQ(WorldFarVolumeIdentity(
                  world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
              identity);
    std::array<std::uint64_t, 7> variants{};
    variants[0] = WorldFarVolumeIdentity(
        world, FarCaveMode::BoxFiltered, FarVolumeNumericProfile::RawFloat, 4);
    variants[1] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::LegacySigned16, 4);
    variants[2] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 8);
    world.set_seed(18);
    variants[3] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4);
    world.set_seed(17);
    params.surface_sampling_revision = 2;
    world.set_params(params);
    variants[4] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4);
    params.surface_sampling_revision = 1;
    auto replacement = std::make_shared<WorldgenDependencySnapshot>(*dependencies);
    replacement->manifest[0][1] = "content-b";
    params.worldgen_dependencies = replacement;
    world.set_params(params);
    variants[5] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4);
    params.height_offset = 1;
    params.worldgen_dependencies = dependencies;
    world.set_params(params);
    variants[6] = WorldFarVolumeIdentity(
        world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4);
    params.height_offset = 0.25f;
    params.worldgen_dependencies = replacement;
    world.set_params(params);
    for (const auto value : variants) {
        EXPECT_NE(value, identity);
    }
    EXPECT_EQ(WorldFarVolumeIdentity(
                  world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 0),
              0u);
    FarFrozenCaptureSource source;
    source.pristine.field_identity = source.content_identity = identity;
    source.use = FarFrozenUse::SessionRender;
    source.session_water_resolution = 4;
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{};
    };
    source.water = [](std::int64_t, std::int64_t) {
        return std::optional{FarFrozenWaterSample{false, 0}};
    };
    auto pool = std::make_shared<FarVolumeMemoryPool>(32u * 1024u * 1024u);
    const FarVolumeRequest request{{1, 0, 0}, {-16, 16}, identity};
    const FarFrozenCaptureLimits limits{1, 100'000, 1'000'000, 100'000, 32u * 1024u * 1024u};
    std::shared_ptr<const FarFrozenObservation> output;
    EXPECT_EQ(CaptureWorldFarVolumeObservation(world, request, source, limits, pool, output),
              FarFrozenCaptureError::IdentityMismatch);
    EXPECT_FALSE(output);
    EXPECT_EQ(pool->attempts(), 0u);
    params.worldgen_dependencies = dependencies;
    world.set_params(params);
    EXPECT_EQ(CaptureWorldFarVolumeObservation(world, request, source, limits, pool, output),
              FarFrozenCaptureError::None);
    ASSERT_TRUE(output);
    const auto accepted = output;
    world.set_seed(19);
    EXPECT_EQ(CaptureWorldFarVolumeObservation(world, request, source, limits, pool, output),
              FarFrozenCaptureError::IdentityMismatch);
    EXPECT_EQ(output, accepted);
    params.worldgen_dependencies.reset();
    params.biomes_enabled = true;
    world.set_params(params);
    EXPECT_EQ(WorldFarVolumeIdentity(
                  world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
              0u);
}

TEST(FarVolumeWorld, RejectsMissingRetainedBinding) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeMemoryPool pool(1024);
    FarVolumeCompilation output;
    output.tile.request = PristineHeld();
    const auto previous = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::RawFloat,
                                                 PristineLimits(),
                                                 {&pool, nullptr, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::InvalidMemory);
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(pool.attempts(), 0u);
    EXPECT_EQ(pool.used(), 0u);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeWorld, RefusalPreservesPriorOutputAndCharge) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeMemoryPool pool(1024);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation output;
    output.mesh.vertices.emplace_back();
    output.tile.request = PristineHeld();
    ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
    const auto previous = output;
    const auto held = retained.bytes();
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::LegacySigned16,
                                                 PristineLimits(),
                                                 {&pool, &retained, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    // The first height-cache allocation is refused before any world sampling.
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(pool.refusals(), 1u);
    EXPECT_EQ(retained.bytes(), held);
    EXPECT_EQ(pool.used(), held);
    EXPECT_EQ(output, previous);
}

TEST(FarVolumeWorld, NullPoolKeepsLegacyRefusal) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
    FarVolumeCompilation expected, actual;
    expected.tile.request = actual.tile.request = PristineHeld();
    FarVolumeFacadeFailure uncharged, charged;
    auto limits = PristineLimits();
    limits.authority.max_height_samples = 0;
    EXPECT_FALSE(CompilePristineFarVolume(world,
                                          PristineIntent(),
                                          42,
                                          FarCaveMode::BandLimited,
                                          FarVolumeNumericProfile::RawFloat,
                                          limits,
                                          expected,
                                          &uncharged));
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristineIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::RawFloat,
                                                 limits,
                                                 {},
                                                 actual,
                                                 &charged));
    EXPECT_EQ(uncharged.code, FarVolumeFacadeError::HeightLimit);
    EXPECT_EQ(charged.code, uncharged.code);
    EXPECT_EQ(charged.work, uncharged.work);
    EXPECT_EQ(actual, expected);
}

TEST(FarVolumeWorld, SuccessfulChargedCompileMatchesUnchargedAndNullPool) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    const auto intent = PristinePlaneIntent();
    const auto limits = PristinePlaneLimits();
    FarVolumeMemoryPool pool(64u * 1024u * 1024u);
    FarVolumeMemoryLease retained(pool);
    FarVolumeCompilation output;
    output.tile.request = PristineHeld();
    output.mesh.vertices.emplace_back();
    ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
    FarVolumeFacadeFailure failure;
    ASSERT_TRUE(CompilePristineFarVolumeCharged(world,
                                                intent,
                                                42,
                                                FarCaveMode::BandLimited,
                                                FarVolumeNumericProfile::RawFloat,
                                                limits,
                                                {&pool, &retained, nullptr},
                                                output,
                                                &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(failure.generation, FarVolumeBuildError::None);
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::None);
    EXPECT_EQ(output.intent, intent);
    EXPECT_EQ(output.caves, FarCaveMode::BandLimited);
    EXPECT_EQ(output.numeric, FarVolumeNumericProfile::RawFloat);
    EXPECT_EQ(output.tile.request, (FarVolumeRequest{intent.key, {-256, 256}, 42}));
    EXPECT_EQ(output.tile.min_meters, (FarVolumePosition{0, -256, 0}));
    EXPECT_EQ(output.tile.max_meters, (FarVolumePosition{8192, 256, 8192}));
    EXPECT_EQ(output.tile.content, FarVolumeContent::Mixed);
    ASSERT_EQ(output.tile.bricks.size(), 32u * 32u);
    EXPECT_EQ(output.tile.candidates_visited, 2u * 32u * 32u);
    EXPECT_EQ(output.tile.samples_evaluated,
              output.tile.candidates_visited * kFarVolumeBrickSamples);
    EXPECT_EQ(output.mesh.request, output.tile.request);
    EXPECT_EQ(output.mesh.origin_meters, output.tile.min_meters);
    EXPECT_EQ(output.mesh.sampled_max_meters, output.tile.max_meters);
    EXPECT_EQ(output.mesh.content, FarVolumeContent::Mixed);
    EXPECT_EQ(output.mesh.cells_visited, output.tile.bricks.size() * 64u);
    ASSERT_EQ(output.mesh.vertices.size(), 129u * 129u);
    ASSERT_EQ(output.mesh.indices.size(), 128u * 128u * 6u);
    ASSERT_TRUE(output.mesh.bounds.has_value());
    if (output.mesh.bounds) {
        EXPECT_EQ(output.mesh.bounds->min, (FarVolumeVector{0.0, 256.25, 0.0}));
        EXPECT_EQ(output.mesh.bounds->max, (FarVolumeVector{8192.0, 256.25, 8192.0}));
    }
    for (const auto index : output.mesh.indices)
        ASSERT_LT(index, output.mesh.vertices.size());
    EXPECT_EQ(output.work, failure.work);
    EXPECT_GT(output.work.height_samples, 129u * 129u); // Includes the normal halo.
    EXPECT_EQ(output.work.density_samples,
              output.tile.samples_evaluated + output.mesh.samples_evaluated);
    EXPECT_GT(retained.bytes(), 0u);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(output));
    EXPECT_EQ(pool.used(), retained.bytes());
    EXPECT_EQ(pool.refusals(), 0u);

    FarVolumeCompilation uncharged, null_pool;
    FarVolumeFacadeFailure uncharged_failure, null_pool_failure;
    ASSERT_TRUE(CompilePristineFarVolume(world,
                                         intent,
                                         42,
                                         FarCaveMode::BandLimited,
                                         FarVolumeNumericProfile::RawFloat,
                                         limits,
                                         uncharged,
                                         &uncharged_failure));
    ASSERT_TRUE(CompilePristineFarVolumeCharged(world,
                                                intent,
                                                42,
                                                FarCaveMode::BandLimited,
                                                FarVolumeNumericProfile::RawFloat,
                                                limits,
                                                {},
                                                null_pool,
                                                &null_pool_failure));
    EXPECT_EQ(uncharged_failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(null_pool_failure.code, FarVolumeFacadeError::None);
    EXPECT_EQ(uncharged_failure.work, failure.work);
    EXPECT_EQ(null_pool_failure.work, failure.work);
    EXPECT_EQ(output, uncharged);
    EXPECT_EQ(output, null_pool);
    EXPECT_EQ(pool.used(), retained.bytes());
}

TEST(FarVolumeWorld, BrickAllocationRefusalAfterSamplingPreservesPriorOutputAndCharge) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    FarVolumeCompilation output;
    output.mesh.vertices.emplace_back();
    output.tile.request = PristineHeld();
    const auto held = FarVolumeCompilationRetainedBytes(output);
    // Measure discovery's height cache, including allocator bookkeeping.
    FarVolumeMemoryPool discovery_pool(64u * 1024u * 1024u);
    FarVolumeAuthority authority;
    authority.field_identity = 42;
    authority.caves = FarCaveMode::BandLimited;
    authority.numeric = FarVolumeNumericProfile::LegacySigned16;
    authority.height = [&world](std::int64_t x, std::int64_t z) {
        return world.GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
    };
    // Discovery requires a density callback but never invokes it.
    authority.density = [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{};
    };
    FarVolumeRequest discovered;
    {
        const auto scope = world.acquire_worldgen_sample_scope();
        ASSERT_TRUE(ResolveFarVolumeCoverageCharged(PristinePlaneIntent(),
                                                    authority,
                                                    PristinePlaneLimits().authority,
                                                    {&discovery_pool, nullptr, nullptr},
                                                    discovered));
    }
    const auto measured_discovery_peak = discovery_pool.peak();
    // No bytes remain for the first mixed-brick buffer, after two bricks' samples.
    FarVolumeMemoryPool pool(held + measured_discovery_peak);
    FarVolumeMemoryLease retained(pool);
    ASSERT_TRUE(retained.TryCharge(held));
    const auto previous = output;
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeCharged(world,
                                                 PristinePlaneIntent(),
                                                 42,
                                                 FarCaveMode::BandLimited,
                                                 FarVolumeNumericProfile::LegacySigned16,
                                                 PristinePlaneLimits(),
                                                 {&pool, &retained, nullptr},
                                                 output,
                                                 &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_EQ(failure.generation, FarVolumeBuildError::AllocationFailure);
    EXPECT_EQ(failure.mesh, FarVolumeMeshError::None);
    EXPECT_EQ(failure.work.height_samples, 129u * 129u);
    EXPECT_EQ(failure.work.density_samples, 2u * kFarVolumeBrickSamples);
    EXPECT_EQ(pool.refusals(), 1u);
    EXPECT_EQ(pool.peak(), pool.limit());
    EXPECT_EQ(output, previous);
    EXPECT_EQ(retained.bytes(), held);
    EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(output));
    EXPECT_EQ(pool.used(), held);
}

TEST(FarVolumeWorld, ExplicitSpanPristineWrappersMatchDiscoveredCompilationAndPages) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        FarVolumeCompilation discovered;
        ASSERT_TRUE(CompilePristineFarVolume(world,
                                             PristinePlaneIntent(),
                                             42,
                                             FarCaveMode::BandLimited,
                                             profile,
                                             PristinePlaneLimits(),
                                             discovered));
        const auto request = discovered.tile.request;
        FarVolumeCompilation full, null_pool;
        ASSERT_TRUE(CompilePristineFarVolumeSpan(
            world, request, FarCaveMode::BandLimited, profile, PristinePlaneLimits(), full));
        ASSERT_TRUE(CompilePristineFarVolumeSpanCharged(world,
                                                        request,
                                                        FarCaveMode::BandLimited,
                                                        profile,
                                                        PristinePlaneLimits(),
                                                        {},
                                                        null_pool));
        EXPECT_EQ(full, null_pool);
        EXPECT_EQ(full.tile, discovered.tile);
        EXPECT_EQ(full.mesh, discovered.mesh);
        EXPECT_EQ(full.legacy_checksum, discovered.legacy_checksum);
        FarVolumePagePlan plan;
        ASSERT_TRUE(plan.Reset(request, 256, 2));
        std::size_t vertices = 0, indices = 0;
        for (std::uint64_t i = 0; i < plan.size(); ++i) {
            FarVolumeRequest page;
            ASSERT_TRUE(plan.Page(i, page));
            FarVolumeMemoryPool pool(64u * 1024u * 1024u);
            FarVolumeMemoryLease retained(pool);
            FarVolumeCompilation compiled;
            ASSERT_TRUE(CompilePristineFarVolumeSpanCharged(world,
                                                            page,
                                                            FarCaveMode::BandLimited,
                                                            profile,
                                                            PristinePlaneLimits(),
                                                            {&pool, &retained, nullptr},
                                                            compiled));
            EXPECT_EQ(compiled.tile.request, page);
            EXPECT_EQ(compiled.mesh.request, page);
            EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(compiled));
            EXPECT_EQ(pool.used(), retained.bytes());
            vertices += compiled.mesh.vertices.size();
            indices += compiled.mesh.indices.size();
            if (!compiled.mesh.indices.empty()) {
                auto expected_vertices = full.mesh.vertices;
                const auto& origin = compiled.mesh.origin_meters;
                const auto& full_origin = full.mesh.origin_meters;
                for (auto& vertex : expected_vertices) {
                    vertex.position[0] += static_cast<double>(full_origin.x - origin.x);
                    vertex.position[1] += static_cast<double>(full_origin.y - origin.y);
                    vertex.position[2] += static_cast<double>(full_origin.z - origin.z);
                }
                EXPECT_EQ(compiled.mesh.vertices, expected_vertices);
                EXPECT_EQ(compiled.mesh.indices, full.mesh.indices);
            }
        }
        EXPECT_EQ(vertices, full.mesh.vertices.size());
        EXPECT_EQ(indices, full.mesh.indices.size());
    }
}

TEST(FarVolumeWorld, ExplicitSpanRefusalsPreserveOutputAndCharge) {
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, PristinePlaneParams(), 1337);
    FarVolumeMemoryPool pool(64u * 1024u * 1024u), other(1024);
    FarVolumeMemoryLease retained(pool), wrong(other);
    FarVolumeCompilation output;
    output.mesh.vertices.emplace_back();
    output.tile.request = PristineHeld();
    ASSERT_TRUE(retained.TryCharge(FarVolumeCompilationRetainedBytes(output)));
    const auto held = retained.bytes();
    const auto saved = output;
    const FarVolumeRequest request{{5, 0, 0}, {-256, 256}, 42};
    for (unsigned refusal = 0; refusal < 4; ++refusal) {
        auto input = request;
        auto limits = PristinePlaneLimits();
        FarVolumeMemory memory{&pool, &retained, nullptr};
        if (refusal == 0) {
            input.span.max_y = 0;
            input.span.min_y = 0;
        }
        if (refusal == 1) {
            memory.retained = nullptr;
        }
        if (refusal == 2) {
            memory.retained = &wrong;
        }
        if (refusal == 3) {
            limits.generation.max_candidate_bricks = 2047;
        }
        FarVolumeFacadeFailure failure;
        EXPECT_FALSE(CompilePristineFarVolumeSpanCharged(world,
                                                         input,
                                                         FarCaveMode::BandLimited,
                                                         FarVolumeNumericProfile::RawFloat,
                                                         limits,
                                                         memory,
                                                         output,
                                                         &failure));
        EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
        EXPECT_EQ(output, saved);
        EXPECT_EQ(pool.used(), held);
        EXPECT_EQ(retained.bytes(), held);
    }
    pool.RefuseAttemptsFrom(pool.attempts());
    FarVolumeFacadeFailure failure;
    EXPECT_FALSE(CompilePristineFarVolumeSpanCharged(world,
                                                     request,
                                                     FarCaveMode::BandLimited,
                                                     FarVolumeNumericProfile::RawFloat,
                                                     PristinePlaneLimits(),
                                                     {&pool, &retained, nullptr},
                                                     output,
                                                     &failure));
    EXPECT_EQ(failure.code, FarVolumeFacadeError::AllocationFailure);
    EXPECT_EQ(failure.work, FarVolumeAuthorityWork{});
    EXPECT_EQ(output, saved);
    EXPECT_EQ(pool.used(), held);
    EXPECT_EQ(retained.bytes(), held);
}

namespace {
FarVolumeCompilationLimits SpanGeometryLimits() {
    return {{131 * 131, 20'000'000}, {131'072, 16'384}, {1'048'576, 3'000'000, 300'000, 1'000'000}};
}
FarVolumeAuthority SpanGeometryAuthority(FarVolumeNumericProfile profile) {
    return {42,
            FarCaveMode::BandLimited,
            profile,
            [](std::int64_t, std::int64_t) { return 0.0f; },
            [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                return FarVolumeSample{static_cast<float>(p.y) + .25f, 7};
            }};
}
using SpanVertex = std::array<std::int64_t, 7>;
using SpanTriangle = std::array<SpanVertex, 3>;

std::vector<SpanTriangle> SpanTriangles(const FarVolumeMesh& mesh) {
    std::vector<SpanTriangle> triangles;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        SpanTriangle triangle{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& vertex = mesh.vertices.at(mesh.indices[i + corner]);
            const std::array<std::int64_t, 3> origin{
                mesh.origin_meters.x, mesh.origin_meters.y, mesh.origin_meters.z};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                // Compare world geometry to 1e-8 m, independent of page origins.
                triangle[corner][axis] =
                    std::llround((vertex.position[axis] + static_cast<double>(origin[axis])) * 1e8);
                triangle[corner][axis + 3] = std::llround(vertex.normal[axis] * 1e8);
            }
            triangle[corner][6] = vertex.material;
        }
        // Rotate, preserving triangle winding and multiplicity.
        std::rotate(
            triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

void ExpectSpanPages(const FarVolumeRequest& request,
                     const FarVolumeAuthority& authority,
                     std::int64_t stride) {
    FarVolumeCompilation full;
    ASSERT_TRUE(CompileFarVolumeSpan(request, authority, SpanGeometryLimits(), full));
    ASSERT_FALSE(full.mesh.indices.empty());
    FarVolumePagePlan plan;
    ASSERT_TRUE(plan.Reset(request, stride, 32));
    FarVolumePageCompleteness completeness(plan);
    std::vector<SpanTriangle> triangles;
    std::vector<FarVolumeBrick> bricks;
    std::uint64_t candidates = 0, samples = 0;
    for (std::uint64_t i = 0; i < plan.size(); ++i) {
        FarVolumeRequest page;
        ASSERT_TRUE(plan.Page(i, page));
        FarVolumeCompilation compiled;
        FarVolumeMemoryPool pool(128u * 1024u * 1024u);
        FarVolumeMemoryLease retained(pool);
        ASSERT_TRUE(CompileFarVolumeSpanCharged(
            page, authority, SpanGeometryLimits(), {&pool, &retained, nullptr}, compiled));
        EXPECT_EQ(compiled.tile.request, page);
        EXPECT_EQ(compiled.mesh.request, page);
        EXPECT_EQ(retained.bytes(), FarVolumeCompilationRetainedBytes(compiled));
        EXPECT_EQ(pool.used(), retained.bytes());
        const auto page_triangles = SpanTriangles(compiled.mesh);
        triangles.insert(triangles.end(), page_triangles.begin(), page_triangles.end());
        bricks.insert(bricks.end(), compiled.tile.bricks.begin(), compiled.tile.bricks.end());
        candidates += compiled.tile.candidates_visited;
        samples += compiled.tile.samples_evaluated;
        ASSERT_TRUE(completeness.Acknowledge(page));
    }
    EXPECT_TRUE(completeness.Complete());
    std::sort(triangles.begin(), triangles.end());
    EXPECT_EQ(triangles, SpanTriangles(full.mesh));
    std::sort(bricks.begin(), bricks.end(), [](const auto& a, const auto& b) {
        return std::tie(a.origin_meters.z, a.origin_meters.x, a.origin_meters.y) <
               std::tie(b.origin_meters.z, b.origin_meters.x, b.origin_meters.y);
    });
    EXPECT_EQ(bricks, full.tile.bricks);
    EXPECT_EQ(candidates, full.tile.candidates_visited);
    EXPECT_EQ(samples, full.tile.samples_evaluated);
}

TEST(FarVolumeWorld, ExplicitSpanPagesMatchFullGeometryAndHaloNormalsAtFacesEdgesCorners) {
    auto authority = SpanGeometryAuthority(FarVolumeNumericProfile::RawFloat);
    authority.density = [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
        const auto x = static_cast<float>(p.x), z = static_cast<float>(p.z);
        return FarVolumeSample{static_cast<float>(p.y) + .25f + (x * x + z * z) / 8192, 7};
    };
    std::uint64_t y_faces = 0, xz_edges = 0, corners = 0;
    for (const std::int64_t x : {-1, 0}) {
        for (const std::int64_t z : {-1, 0}) {
            const FarVolumeRequest request{{1, x, z}, {-80, 16}, 42};
            ExpectSpanPages(request, authority, 32);
            FarVolumePagePlan plan;
            ASSERT_TRUE(plan.Reset(request, 32, 4));
            for (std::uint64_t i = 0; i < plan.size(); ++i) {
                FarVolumeRequest page;
                ASSERT_TRUE(plan.Page(i, page));
                FarVolumeCompilation compiled;
                ASSERT_TRUE(CompileFarVolumeSpan(page, authority, SpanGeometryLimits(), compiled));
                for (const auto& vertex : compiled.mesh.vertices) {
                    const double wx =
                        vertex.position[0] + static_cast<double>(compiled.mesh.origin_meters.x);
                    const double wy =
                        vertex.position[1] + static_cast<double>(compiled.mesh.origin_meters.y);
                    const double wz =
                        vertex.position[2] + static_cast<double>(compiled.mesh.origin_meters.z);
                    const double gx = wx / 4096, gz = wz / 4096;
                    const double length = std::sqrt(gx * gx + 1 + gz * gz);
                    EXPECT_NEAR(vertex.normal[0], gx / length, 1e-12);
                    EXPECT_NEAR(vertex.normal[1], 1 / length, 1e-12);
                    EXPECT_NEAR(vertex.normal[2], gz / length, 1e-12);
                    y_faces += wy == static_cast<double>(page.span.min_y) ||
                                       wy == static_cast<double>(page.span.max_y)
                                   ? 1u
                                   : 0u;
                    xz_edges += wx == 0 || wz == 0 ? 1u : 0u;
                    corners += wx == 0 && wz == 0 ? 1u : 0u;
                }
            }
        }
    }
    EXPECT_GT(y_faces, 0u);
    EXPECT_GT(xz_edges, 0u);
    EXPECT_GT(corners, 0u);
}

TEST(FarVolumeWorld, ExplicitSpanTorusCrossesZeroWithEqualPagedOutput) {
    for (const auto profile :
         {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
        auto authority = SpanGeometryAuthority(profile);
        authority.density = [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
            const double x = static_cast<double>(p.x) - 32;
            const double z = static_cast<double>(p.z) - 32;
            const double ring = std::sqrt(x * x + z * z) - 20;
            const double y = static_cast<double>(p.y);
            return FarVolumeSample{static_cast<float>(std::sqrt(ring * ring + y * y) - 9),
                                   static_cast<std::uint8_t>(p.y < 0 ? 3 : 7)};
        };
        ExpectSpanPages({{1, 0, 0}, {-32, 32}, 42}, authority, 16);
    }
}

} // namespace
