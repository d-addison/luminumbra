//  GPU particle framework determinism unit tests.
//
// These exercise the SIM-DETERMINISTIC emitter-descriptor surface WITHOUT a GL
// context (no buffers/shaders are created): the descriptor schedule is a pure
// function of world state, and per-particle MOTION is render-only and never
// snapshotted (regression review). The byte-equal and seed-derivation guarantees
// asserted here are exactly what the ParticleEmitterDeterminism gate relies on.

#include "rendering/passes/ParticlePass.h"

#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

// Replace only the GL boundary used by init_buffers/destroy_buffers. CPU storage,
// emission, integration and ownership all run through the public production API.
template<typename Result, typename... Args>
Result GLAPIENTRY IgnoreParticleGlCall(Args...) {
    return Result();
}

template<typename Function>
class ScopedParticleGlCall {
public:
    explicit ScopedParticleGlCall(Function& entry, Function replacement = IgnoreParticleGlCall)
        : m_entry(entry)
        , m_original(entry) {
        m_entry = replacement;
    }
    ~ScopedParticleGlCall() {
        m_entry = m_original;
    }
    ScopedParticleGlCall(const ScopedParticleGlCall&) = delete;
    ScopedParticleGlCall& operator=(const ScopedParticleGlCall&) = delete;

private:
    Function& m_entry;
    Function m_original;
};

class ParticleLiveTest : public testing::Test {
protected:
    void SetUp() override {
        pass.init_buffers();
        EXPECT_EQ(pass.live_particle_count(), 0u);
        EXPECT_TRUE(pass.live_particle_states().empty());
    }
    void TearDown() override {
        pass.destroy_buffers();
        EXPECT_EQ(pass.live_particle_count(), 0u);
        EXPECT_TRUE(pass.live_particle_states().empty());
    }
    static void GLAPIENTRY GenerateNames(GLsizei count, GLuint* names) {
        std::fill_n(names, count, 0u); // No GPU objects; labels/deletion are no-ops.
    }
    static void* GLAPIENTRY MapInstances(GLenum, GLintptr offset, GLsizeiptr bytes, GLbitfield) {
        // One bounded CPU mapping suffices: update writes only one ring per call.
        static std::array<ParticlePass::InstanceRecord, ParticlePass::kMaxInstances> records{};
        EXPECT_EQ(offset, 0);
        EXPECT_EQ(bytes, static_cast<GLsizeiptr>(sizeof(records)));
        return records.data();
    }

    ParticlePass pass;
    const fs::path particle_root = SourceRoot() / "data/common/particles";
    const fs::path spray_file = particle_root / "waterfall_spray.json";

private:
    ScopedParticleGlCall<PFNGLGENVERTEXARRAYSPROC> gen_arrays{glad_glGenVertexArrays,
                                                              GenerateNames};
    ScopedParticleGlCall<PFNGLGENBUFFERSPROC> gen_buffers{glad_glGenBuffers, GenerateNames};
    ScopedParticleGlCall<PFNGLBINDVERTEXARRAYPROC> bind_array{glad_glBindVertexArray};
    ScopedParticleGlCall<PFNGLBINDBUFFERPROC> bind_buffer{glad_glBindBuffer};
    ScopedParticleGlCall<PFNGLBUFFERSTORAGEPROC> storage{glad_glBufferStorage};
    ScopedParticleGlCall<PFNGLMAPBUFFERRANGEPROC> mapping{glad_glMapBufferRange, MapInstances};
    ScopedParticleGlCall<PFNGLVERTEXBINDINGDIVISORPROC> divisor{glad_glVertexBindingDivisor};
    ScopedParticleGlCall<PFNGLENABLEVERTEXATTRIBARRAYPROC> enable_attribute{
        glad_glEnableVertexAttribArray};
    ScopedParticleGlCall<PFNGLVERTEXATTRIBFORMATPROC> attribute_format{glad_glVertexAttribFormat};
    ScopedParticleGlCall<PFNGLVERTEXATTRIBBINDINGPROC> attribute_binding{
        glad_glVertexAttribBinding};
    ScopedParticleGlCall<PFNGLVERTEXATTRIBIFORMATPROC> integer_format{glad_glVertexAttribIFormat};
    ScopedParticleGlCall<PFNGLBINDVERTEXBUFFERPROC> vertex_buffer{glad_glBindVertexBuffer};
};

void ExpectParticleStates(const ParticlePass& pass,
                          const std::vector<ParticlePass::LiveParticleState>& expected) {
    const auto actual = pass.live_particle_states();
    ASSERT_EQ(actual.size(), expected.size());
    EXPECT_EQ(pass.live_particle_count(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].pos, expected[i].pos);
        EXPECT_EQ(actual[i].vel, expected[i].vel);
        EXPECT_EQ(actual[i].age, expected[i].age);
        EXPECT_EQ(actual[i].lifetime, expected[i].lifetime);
        EXPECT_EQ(actual[i].emitter_index, expected[i].emitter_index);
    }
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

TEST_F(ParticleLiveTest, WaterfallRemovalPreservesAndRemapsEveryUnrelatedParticle) {
    ASSERT_EQ(pass.add_emitter(FixtureEmitter(), glm::vec3(1.0f)), 0u);
    ASSERT_EQ(pass.add_waterfall_spray(spray_file, glm::vec3(20.0f), 8.0f, 4.0f), 1u);
    ASSERT_EQ(pass.add_splash_emitter(particle_root / "precip_splash.json"), 2u);
    ASSERT_EQ(pass.add_waterfall_spray(spray_file, glm::vec3(40.0f), 8.0f, 4.0f), 3u);
    ASSERT_EQ(pass.add_emitter(FixtureEmitter(), glm::vec3(3.0f)), 4u);
    pass.set_precipitation(particle_root, glm::vec3(0.0f, -100.0f, 0.0f), 1.0f, 1.0f);
    pass.rebuild_emitter_descriptors(777u, 10u);
    // Low rain makes live splash particles; high rain then leaves live drops too.
    pass.update(0.125f);
    pass.set_precipitation(particle_root, glm::vec3(0.0f, 100.0f, 0.0f), 1.0f, 1.0f);
    pass.update(0.125f);
    pass.update(0.0f); // Include the previous frame's impact bursts in the live count.
    const auto before = pass.live_particle_states();
    ASSERT_EQ(pass.live_particle_count(), before.size());
    std::array<std::size_t, 7> owned{};
    std::vector<ParticlePass::LiveParticleState> survivors;
    constexpr std::array<uint32_t, 7> remap{
        0, ParticlePass::kInvalidEmitter, 1, ParticlePass::kInvalidEmitter, 2, 3, 4};
    for (auto particle : before) {
        ASSERT_LT(particle.emitter_index, owned.size());
        ++owned[particle.emitter_index];
        const auto mapped = remap[particle.emitter_index];
        if (mapped != ParticlePass::kInvalidEmitter) {
            particle.emitter_index = mapped;
            survivors.push_back(particle);
        }
    }
    for (const auto count : owned)
        ASSERT_GT(count, 0u); // Every emitter, including both falls, rain, snow and splash.
    ASSERT_LT(survivors.size(), before.size());

    pass.clear_waterfall_spray();
    ASSERT_NO_FATAL_FAILURE(ExpectParticleStates(pass, survivors));
    pass.clear_waterfall_spray();
    ASSERT_NO_FATAL_FAILURE(ExpectParticleStates(pass, survivors));

    // Precipitation handles still address the compacted rain/snow emitters.
    pass.set_precipitation(particle_root, glm::vec3(9.0f, 100.0f, 9.0f), 0.0f, 0.0f);
    pass.rebuild_emitter_descriptors(777u, 10u);
    ASSERT_EQ(pass.emitter_descriptors().size(), 5u);
    EXPECT_EQ(pass.emitter_descriptors()[3].id, 5u);
    EXPECT_EQ(pass.emitter_descriptors()[4].id, 6u);
    EXPECT_EQ(pass.emitter_descriptors()[3].origin_region[0], 9000);
    EXPECT_EQ(pass.emitter_descriptors()[4].origin_region[0], 9000);
    pass.update(0.03125f);
    const auto disabled_weather = pass.live_particle_states();
    EXPECT_EQ(pass.live_particle_count(), disabled_weather.size());
    for (uint32_t index = 3; index < 5; ++index) {
        EXPECT_EQ(static_cast<std::size_t>(std::count_if(
                      disabled_weather.begin(),
                      disabled_weather.end(),
                      [index](const auto& particle) { return particle.emitter_index == index; })),
                  owned[index + 2]); // Both weather spawn scales are zero; old drops survive.
    }

    // A new impact must use the remapped splash template, now emitter index 1.
    pass.set_precipitation(particle_root, glm::vec3(0.0f, -100.0f, 0.0f), 1.0f, 0.0f);
    pass.update(0.03125f);
    pass.update(0.0f);
    const auto after_impact = pass.live_particle_states();
    EXPECT_EQ(pass.live_particle_count(), after_impact.size());
    EXPECT_GT(std::count_if(after_impact.begin(),
                            after_impact.end(),
                            [](const auto& particle) {
                                return particle.emitter_index == 1u && particle.age == 0.0f;
                            }),
              0);

    pass.clear_emitters();
    ExpectParticleStates(pass, {});
    EXPECT_FALSE(pass.has_emitters());
    pass.set_precipitation(particle_root, glm::vec3(100.0f), 1.0f, 1.0f);
    pass.rebuild_emitter_descriptors(777u, 10u);
    ASSERT_EQ(pass.emitter_descriptors().size(), 2u); // Cached handles/load flags reset.
    EXPECT_EQ(pass.emitter_descriptors()[0].id, 0u);
    EXPECT_EQ(pass.emitter_descriptors()[1].id, 1u);
    pass.update(0.125f); // Teardown also checks destroy_buffers with live particles.
    EXPECT_GT(pass.live_particle_count(), 0u);
}

TEST_F(ParticleLiveTest, WaterfallOnlyLiveSetBecomesEmpty) {
    for (int site = 0; site < 2; ++site)
        ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(20.0f), 8.0f, 4.0f),
                  ParticlePass::kInvalidEmitter);
    pass.rebuild_emitter_descriptors(777u, 10u);
    pass.update(0.125f);
    ASSERT_GT(pass.live_particle_count(), 0u);
    const auto before = pass.live_particle_states();
    for (uint32_t index = 0; index < 2; ++index)
        ASSERT_TRUE(std::any_of(before.begin(), before.end(), [index](const auto& particle) {
            return particle.emitter_index == index;
        }));
    pass.clear_waterfall_spray();
    ExpectParticleStates(pass, {});
    EXPECT_FALSE(pass.has_emitters());
    pass.clear_waterfall_spray();
    ExpectParticleStates(pass, {});
    pass.update(0.125f);
    ExpectParticleStates(pass, {});
}

TEST_F(ParticleLiveTest, NoWaterfallParticlesLeavesLiveSetUnchanged) {
    ASSERT_EQ(pass.add_emitter(FixtureEmitter(), glm::vec3(1.0f)), 0u);
    pass.rebuild_emitter_descriptors(777u, 10u);
    pass.update(0.125f);
    const auto before = pass.live_particle_states();
    ASSERT_FALSE(before.empty());
    pass.clear_waterfall_spray(); // No waterfall emitters: early return.
    ExpectParticleStates(pass, before);
    ASSERT_EQ(pass.add_waterfall_spray(spray_file, glm::vec3(20.0f), 8.0f, 4.0f), 1u);
    pass.clear_waterfall_spray(); // A waterfall emitter exists but has never spawned.
    ExpectParticleStates(pass, before);
    pass.clear_waterfall_spray();
    ExpectParticleStates(pass, before);
}
