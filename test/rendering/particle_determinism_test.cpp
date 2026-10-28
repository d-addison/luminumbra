//  GPU particle framework determinism unit tests.
//
// These exercise the SIM-DETERMINISTIC emitter-descriptor surface WITHOUT a GL
// context (no buffers/shaders are created): the descriptor schedule is a pure
// function of world state, and per-particle MOTION is render-only and never
// snapshotted (regression review). The byte-equal and seed-derivation guarantees
// asserted here are exactly what the ParticleEmitterDeterminism gate relies on.

#include "rendering/passes/ParticlePass.h"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>

namespace {

using Luminumbra::Rendering::ParticlePass;
namespace fs = std::filesystem;

fs::path SourceRoot() {
#ifdef LUMINUMBRA_SOURCE_ROOT
    return fs::path(LUMINUMBRA_SOURCE_ROOT);
#else
    return fs::current_path();
#endif
}

fs::path FixtureEmitter() {
    return SourceRoot() / "data/common/particles/fixture_sparkle.json";
}

} // namespace

// Instance record stays a fixed 24-byte POD (pinned stride, documented design).
TEST(ParticleDeterminism, InstanceRecordIs24Bytes) {
    EXPECT_EQ(sizeof(ParticlePass::InstanceRecord), 24u);
    EXPECT_EQ(ParticlePass::kInstanceStride, 24u);
}

// The emitter descriptor stays a fixed-size POD so the snapshot is byte-stable.
TEST(ParticleDeterminism, EmitterDescriptorIsFixedSizePod) {
    EXPECT_EQ(sizeof(ParticlePass::EmitterDescriptor), 36u);
    static_assert(std::is_trivially_copyable<ParticlePass::EmitterDescriptor>::value,
                  "descriptor must be trivially copyable for byte-equal snapshots");
}

// Pinned capacities (documented design).
TEST(ParticleDeterminism, PinnedCapacities) {
    EXPECT_EQ(ParticlePass::kMaxInstances, 65536u);
    EXPECT_EQ(ParticlePass::kMaxEmitters, 256u);
}

// RNG seed is a PURE function of world state + emitter id: identical inputs ->
// identical seed; any input change -> different seed.
TEST(ParticleDeterminism, SeedDerivationIsPureAndSensitive) {
    const uint64_t seed_a = ParticlePass::derive_emitter_seed(1234u, 50u, 7u);
    const uint64_t seed_a2 = ParticlePass::derive_emitter_seed(1234u, 50u, 7u);
    EXPECT_EQ(seed_a, seed_a2); // pure

    EXPECT_NE(seed_a, ParticlePass::derive_emitter_seed(1235u, 50u, 7u)); // seed
    EXPECT_NE(seed_a, ParticlePass::derive_emitter_seed(1234u, 51u, 7u)); // tick
    EXPECT_NE(seed_a, ParticlePass::derive_emitter_seed(1234u, 50u, 8u)); // id
}

// The emitter DESCRIPTOR SET rebuilt twice from identical world state is
// byte-equal (the load-bearing determinism guarantee for the gate). Requires
// the fixture emitter on disk; add_emitter is GL-free (only parses JSON).
TEST(ParticleDeterminism, DescriptorSetIsByteEqualAcrossRuns) {
    ASSERT_TRUE(fs::exists(FixtureEmitter())) << FixtureEmitter().string();

    ParticlePass pass;
    const uint32_t id = pass.add_emitter(FixtureEmitter(), glm::vec3(3.0f, 2.0f, -5.0f));
    ASSERT_NE(id, ParticlePass::kInvalidEmitter);

    const uint64_t world_seed = 0xABCDEF12u;
    const uint64_t world_tick = 4242u;

    pass.rebuild_emitter_descriptors(world_seed, world_tick);
    const std::vector<ParticlePass::EmitterDescriptor> run_a = pass.emitter_descriptors();
    const uint64_t hash_a = pass.emitter_descriptor_hash();

    pass.rebuild_emitter_descriptors(world_seed, world_tick);
    const std::vector<ParticlePass::EmitterDescriptor> run_b = pass.emitter_descriptors();
    const uint64_t hash_b = pass.emitter_descriptor_hash();

    ASSERT_EQ(run_a.size(), run_b.size());
    ASSERT_EQ(run_a.size(), 1u);
    EXPECT_EQ(hash_a, hash_b);
    EXPECT_EQ(0,
              std::memcmp(run_a.data(),
                          run_b.data(),
                          run_a.size() * sizeof(ParticlePass::EmitterDescriptor)));

    // The descriptor's rng_seed must match the documented derivation.
    EXPECT_EQ(run_a[0].rng_seed,
              ParticlePass::derive_emitter_seed(world_seed, world_tick, run_a[0].id));
    EXPECT_EQ(run_a[0].enable, 1u); // fixture has spawn_rate > 0
}

// A different world tick changes the descriptor set hash (the schedule tracks
// world state).
TEST(ParticleDeterminism, DescriptorSetTracksWorldState) {
    ASSERT_TRUE(fs::exists(FixtureEmitter()));
    ParticlePass pass;
    ASSERT_NE(pass.add_emitter(FixtureEmitter(), glm::vec3(0.0f)), ParticlePass::kInvalidEmitter);

    pass.rebuild_emitter_descriptors(99u, 1u);
    const uint64_t hash_tick_1 = pass.emitter_descriptor_hash();
    pass.rebuild_emitter_descriptors(99u, 2u);
    const uint64_t hash_tick_2 = pass.emitter_descriptor_hash();

    EXPECT_NE(hash_tick_1, hash_tick_2);
}

TEST(ParticleDeterminism, WaterfallReplacementPreservesOtherEmitterHandles) {
    ParticlePass pass;
    pass.clear_waterfall_spray();
    EXPECT_FALSE(pass.has_emitters());
    const auto spray_file = SourceRoot() / "data/common/particles/waterfall_spray.json";
    EXPECT_EQ(pass.add_waterfall_spray(spray_file / "missing.json", glm::vec3(0.0f), 8.0f, 4.0f),
              ParticlePass::kInvalidEmitter);
    pass.clear_waterfall_spray();
    EXPECT_FALSE(pass.has_emitters());
    const auto first = pass.add_emitter(FixtureEmitter(), glm::vec3(1.0f));
    ASSERT_NE(first, ParticlePass::kInvalidEmitter);
    ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(20.0f), 8.0f, 4.0f),
              ParticlePass::kInvalidEmitter);
    const auto middle = pass.add_splash_emitter(FixtureEmitter());
    ASSERT_NE(middle, ParticlePass::kInvalidEmitter);
    ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(40.0f), 8.0f, 4.0f),
              ParticlePass::kInvalidEmitter);
    const auto last = pass.add_emitter(FixtureEmitter(), glm::vec3(3.0f));
    ASSERT_NE(last, ParticlePass::kInvalidEmitter);
    pass.set_precipitation(SourceRoot() / "data/common/particles", glm::vec3(5.0f), 1.0f, 1.0f);
    pass.rebuild_emitter_descriptors(777u, 10u);
    ASSERT_EQ(pass.emitter_descriptors().size(), 7u);
    std::vector<ParticlePass::EmitterDescriptor> unrelated;
    for (const auto& descriptor : pass.emitter_descriptors()) {
        if (descriptor.id == first || descriptor.id == middle || descriptor.id >= last)
            unrelated.push_back(descriptor);
    }
    ASSERT_EQ(unrelated.size(), 5u);
    for (int replacement = 0; replacement < 3; ++replacement) {
        pass.clear_waterfall_spray();
        pass.clear_waterfall_spray(); // Empty site sets and repeated teardown are harmless.
        pass.rebuild_emitter_descriptors(777u, 10u);
        ASSERT_EQ(pass.emitter_descriptors().size(), unrelated.size());
        EXPECT_EQ(0,
                  std::memcmp(pass.emitter_descriptors().data(),
                              unrelated.data(),
                              unrelated.size() * sizeof(ParticlePass::EmitterDescriptor)));
        for (int site = 0; site < 24; ++site) {
            ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(60.0f), 8.0f, 4.0f),
                      ParticlePass::kInvalidEmitter);
        }
        pass.rebuild_emitter_descriptors(777u, 10u);
        EXPECT_EQ(pass.emitter_descriptors().size(), unrelated.size() + 24);
    }
    pass.clear_waterfall_spray();
    pass.set_emitter_origin(last, glm::vec3(7.0f));
    pass.set_precipitation(SourceRoot() / "data/common/particles", glm::vec3(9.0f), 1.0f, 1.0f);
    pass.rebuild_emitter_descriptors(777u, 10u);
    ASSERT_EQ(pass.emitter_descriptors().size(), 5u);
    EXPECT_EQ(pass.emitter_descriptors()[2].id, last);
    EXPECT_EQ(pass.emitter_descriptors()[2].origin_region[0], 7000);
    EXPECT_EQ(pass.emitter_descriptors()[3].origin_region[0], 9000);
    EXPECT_EQ(pass.emitter_descriptors()[4].origin_region[0], 9000);
    pass.clear_emitters();
    pass.clear_waterfall_spray();
    EXPECT_FALSE(pass.has_emitters());
    ASSERT_EQ(pass.add_waterfall_spray(spray_file, glm::vec3(0.0f), 8.0f, 4.0f), 0u);
    pass.clear_waterfall_spray();
    EXPECT_FALSE(pass.has_emitters());
}
