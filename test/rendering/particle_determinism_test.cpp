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
