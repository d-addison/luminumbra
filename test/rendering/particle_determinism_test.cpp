//  GPU particle framework determinism unit tests.
//
// These exercise the SIM-DETERMINISTIC emitter-descriptor surface WITHOUT a GL
// context (no buffers/shaders are created): the descriptor schedule is a pure
// function of world state, and per-particle MOTION is render-only and never
// snapshotted (regression review). The byte-equal and seed-derivation guarantees
// asserted here are exactly what the ParticleEmitterDeterminism gate relies on.

#include "../support/UniqueTempPath.h"
#include "rendering/passes/ParticlePass.h"

#include <glad/glad.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
    struct MappedStorage {
        std::array<ParticlePass::InstanceRecord, ParticlePass::kMaxInstances> records{};
    };
    static MappedStorage& Mapping() {
        // One bounded CPU mapping suffices: update writes only one ring per call.
        static MappedStorage storage;
        return storage;
    }
    static void* GLAPIENTRY MapInstances(GLenum, GLintptr offset, GLsizeiptr bytes, GLbitfield) {
        EXPECT_EQ(offset, 0);
        EXPECT_EQ(bytes, static_cast<GLsizeiptr>(sizeof(MappedStorage)));
        return Mapping().records.data();
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

// Independent full-pool oracle: holes are visited and rejected in slot order.
// Both the pool and every per-frame output are bounded by kMaxInstances.
class FullPoolReference {
public:
    struct Emitter {
        ParticlePass::EmitterData data;
        uint64_t rng = 0;
        double credit = 0.0;
    };
    struct Slot {
        ParticlePass::LiveParticleState state;
        bool alive = false;
    };
    std::vector<Emitter> emitters;
    std::vector<Slot> slots = std::vector<Slot>(ParticlePass::kMaxInstances);
    std::vector<ParticlePass::InstanceRecord> records;
    std::size_t head = 0;
    std::size_t visits = 0;
    std::size_t participating = 0;
    std::size_t impact_count = 0;
    std::size_t splash_count = 0;
    glm::vec3 wind{0.0f};

    static float Unit(uint64_t& rng) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return static_cast<float>(rng >> 40) / 16777216.0f;
    }
    static float Signed(uint64_t& rng) {
        return Unit(rng) * 2.0f - 1.0f;
    }
    void Spawn(std::size_t index, const glm::vec3* impact = nullptr) {
        auto& emitter = emitters[index];
        auto& slot = slots[head];
        slot.alive = true;
        slot.state.age = 0.0f;
        slot.state.lifetime = emitter.data.lifetime;
        slot.state.emitter_index = static_cast<uint32_t>(index);
        if (impact) {
            slot.state.pos =
                *impact +
                glm::vec3(Signed(emitter.rng) * 0.15f, 0.02f, Signed(emitter.rng) * 0.15f);
            slot.state.vel =
                emitter.data.base_velocity +
                glm::vec3(Signed(emitter.rng), Unit(emitter.rng), Signed(emitter.rng)) *
                    emitter.data.velocity_jitter;
            ++splash_count;
        } else {
            slot.state.pos =
                emitter.data.origin + glm::vec3(Signed(emitter.rng) * emitter.data.origin_extent.x,
                                                Signed(emitter.rng) * emitter.data.origin_extent.y,
                                                Signed(emitter.rng) * emitter.data.origin_extent.z);
            slot.state.vel =
                emitter.data.base_velocity +
                glm::vec3(Signed(emitter.rng), Signed(emitter.rng), Signed(emitter.rng)) *
                    emitter.data.velocity_jitter;
        }
        head = (head + 1) % slots.size();
    }
    void Update(float dt) {
        for (std::size_t index = 0; index < emitters.size(); ++index) {
            auto& emitter = emitters[index];
            emitter.credit +=
                static_cast<double>(emitter.data.spawn_rate) * static_cast<double>(dt);
            const auto count = static_cast<std::size_t>(emitter.credit);
            emitter.credit -= static_cast<double>(count);
            for (std::size_t spawn = 0; spawn < std::min(count, slots.size()); ++spawn) {
                Spawn(index);
            }
        }
        records.clear();
        visits = 0;
        participating = 0;
        std::vector<glm::vec3> impacts;
        for (auto& slot : slots) {
            ++visits;
            if (!slot.alive) {
                continue;
            }
            ++participating;
            auto& particle = slot.state;
            particle.age += dt;
            if (particle.age >= particle.lifetime) {
                slot.alive = false;
                continue;
            }
            const auto& data = emitters[particle.emitter_index].data;
            const auto velocity = particle.vel + wind * data.wind_response;
            particle.pos += velocity * dt;
            if (data.impact_splash && velocity.y < 0.0f && particle.pos.y <= data.impact_plane_y) {
                impacts.emplace_back(particle.pos.x, data.impact_plane_y, particle.pos.z);
                slot.alive = false;
                ++impact_count;
                continue;
            }
            const float t = particle.age / particle.lifetime;
            float rotation = t * 6.2831853f;
            if (data.streak_aspect > 1.0f) {
                rotation = std::atan2(-velocity.x, velocity.y);
            }
            ParticlePass::InstanceRecord record{};
            record.pos[0] = particle.pos.x;
            record.pos[1] = particle.pos.y;
            record.pos[2] = particle.pos.z;
            record.size = data.size_curve.sample(t);
            record.color[0] = Unorm(data.r_curve.sample(t));
            record.color[1] = Unorm(data.g_curve.sample(t));
            record.color[2] = Unorm(data.b_curve.sample(t));
            record.color[3] = Unorm(data.a_curve.sample(t));
            record.atlas_layer = data.atlas_layer;
            const float angle =
                std::clamp(rotation * (1.0f / 3.14159265358979323846f), -1.0f, 1.0f);
            record.rotation = static_cast<int8_t>(std::lround(angle * 127.0f));
            record.streak = Unorm(data.streak_aspect / ParticlePass::kMaxStreakAspect);
            records.push_back(record);
        }
        for (std::size_t index = 0; index < impacts.size(); index += 4) {
            for (int crown = 0; crown < 3; ++crown) {
                Spawn(2, &impacts[index]);
            }
        }
    }
    std::vector<ParticlePass::LiveParticleState> States() const {
        std::vector<ParticlePass::LiveParticleState> states;
        for (const auto& slot : slots) {
            if (slot.alive) {
                states.push_back(slot.state);
            }
        }
        return states;
    }

private:
    static uint8_t Unorm(float value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }
};

void WriteEmitter(const fs::path& path, const ParticlePass::EmitterData& data) {
    const nlohmann::json doc{
        {"spawn_rate", data.spawn_rate},
        {"lifetime", data.lifetime},
        {"origin", {data.origin.x, data.origin.y, data.origin.z}},
        {"origin_extent", {data.origin_extent.x, data.origin_extent.y, data.origin_extent.z}},
        {"base_velocity", {data.base_velocity.x, data.base_velocity.y, data.base_velocity.z}},
        {"velocity_jitter", data.velocity_jitter},
        {"wind_response", data.wind_response},
        {"streak_aspect", data.streak_aspect},
        {"impact_splash", data.impact_splash},
        {"impact_plane_y", data.impact_plane_y},
        {"atlas_layer", data.atlas_layer},
        {"size_curve", data.size_curve.points},
        {"color_r_curve", data.r_curve.points},
        {"color_g_curve", data.g_curve.points},
        {"color_b_curve", data.b_curve.points},
        {"color_a_curve", data.a_curve.points}};
    std::ofstream output(path);
    output << doc.dump();
    output.close();
    ASSERT_TRUE(output);
}

void ExpectStateBits(const ParticlePass& pass, const FullPoolReference& reference) {
    const auto actual = pass.live_particle_states();
    const auto expected = reference.States();
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        SCOPED_TRACE(index);
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_EQ(std::bit_cast<uint32_t>(actual[index].pos[axis]),
                      std::bit_cast<uint32_t>(expected[index].pos[axis]));
            EXPECT_EQ(std::bit_cast<uint32_t>(actual[index].vel[axis]),
                      std::bit_cast<uint32_t>(expected[index].vel[axis]));
        }
        EXPECT_EQ(std::bit_cast<uint32_t>(actual[index].age),
                  std::bit_cast<uint32_t>(expected[index].age));
        EXPECT_EQ(std::bit_cast<uint32_t>(actual[index].lifetime),
                  std::bit_cast<uint32_t>(expected[index].lifetime));
        EXPECT_EQ(actual[index].emitter_index, expected[index].emitter_index);
    }
}

} // namespace

TEST_F(ParticleLiveTest, AmbientFlagPreservesEveryAuthoredLayer) {
    constexpr std::array<uint16_t, 6> layers{0, 1, 2, 3, 4, 65535};
    const Luminumbra::TestSupport::ScopedTempFile fixture("particle-layers-", ".json");
    std::ifstream input(FixtureEmitter());
    ASSERT_TRUE(input);
    nlohmann::json authored;
    input >> authored;
    for (const auto layer : layers) {
        authored["atlas_layer"] = layer;
        std::ofstream output(fixture.Path());
        output << authored.dump();
        output.close();
        ASSERT_TRUE(output);
        ASSERT_NE(pass.add_emitter(fixture.Path(), glm::vec3(0.0f)), ParticlePass::kInvalidEmitter);
    }
    pass.update_ambient_motes(SourceRoot(), glm::vec3(0.0f));
    pass.rebuild_emitter_descriptors(777u, 10u);
    for (int frame = 0; frame < 4; ++frame) {
        pass.update(0.125f); // Exercise every ring and its reuse with mixed emitters.
        const auto states = pass.live_particle_states();
        ASSERT_EQ(states.size(), pass.frame_instance_count());
        std::array<std::size_t, layers.size() + 1> seen{};
        for (std::size_t index = 0; index < states.size(); ++index) {
            const auto emitter = states[index].emitter_index;
            ASSERT_LT(emitter, seen.size());
            ++seen[emitter];
            const bool ambient = emitter == layers.size();
            EXPECT_EQ(Mapping().records[index].atlas_layer, ambient ? 0 : layers[emitter]);
            EXPECT_EQ(Mapping().records[index].rotation == -128, ambient);
        }
        for (const auto count : seen) {
            EXPECT_GT(count, 0u);
        }
    }
}

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
    for (const auto count : owned) {
        ASSERT_GT(count, 0u); // Every emitter, including both falls, rain, snow and splash.
    }
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

TEST_F(ParticleLiveTest, AmbientMotesTickAfterPreviewFullReset) {
    for (const bool reuse_id : {false, true}) {
        SCOPED_TRACE(reuse_id);
        pass.clear_emitters();
        pass.update_ambient_motes(SourceRoot(), glm::vec3(0.0f));
        pass.rebuild_emitter_descriptors(777u, 10u);
        ASSERT_EQ(pass.emitter_descriptors().size(), 1u);
        const auto old_id = pass.emitter_descriptors().front().id;
        pass.update(0.125f);
        ASSERT_GT(pass.live_particle_count(), 0u);

        // Preview weather changes and cancellation both fully clear the shared pass.
        pass.clear_emitters();
        pass.clear_emitters(); // Empty cancellation must also invalidate the cache.
        ExpectParticleStates(pass, {});
        if (reuse_id) {
            ASSERT_EQ(pass.add_waterfall_spray(spray_file, glm::vec3(10.0f), 8.0f, 4.0f), old_id);
        }
        pass.update_ambient_motes(SourceRoot(), glm::vec3(100.0f, 0.0f, 0.0f));
        pass.update_ambient_motes(SourceRoot(), glm::vec3(200.0f, 0.0f, 0.0f));
        pass.rebuild_emitter_descriptors(777u, 10u);
        ASSERT_EQ(pass.emitter_descriptors().size(), reuse_id ? 2u : 1u);
        const auto ambient_index = reuse_id ? 1u : 0u;
        const auto& ambient = pass.emitter_descriptors().back();
        EXPECT_EQ(ambient.origin_region[0], 200000);
        EXPECT_EQ(ambient.origin_region[1], 5000);
        if (reuse_id) {
            EXPECT_NE(ambient.id, old_id);
            EXPECT_EQ(pass.emitter_descriptors().front().id, old_id);
            EXPECT_EQ(pass.emitter_descriptors().front().origin_region[0], 10000);
        }

        pass.update(0.125f);
        const auto first_tick = pass.live_particle_states();
        ASSERT_EQ(std::count_if(
                      first_tick.begin(),
                      first_tick.end(),
                      [ambient_index](const auto& p) { return p.emitter_index == ambient_index; }),
                  4); // 38 motes per second, with the fractional spawn credit retained.
        pass.update(0.125f);
        const auto second_tick = pass.live_particle_states();
        EXPECT_EQ(pass.frame_instance_count(), second_tick.size());
        EXPECT_EQ(std::count_if(
                      second_tick.begin(),
                      second_tick.end(),
                      [ambient_index](const auto& p) { return p.emitter_index == ambient_index; }),
                  9);
        EXPECT_EQ(std::count_if(second_tick.begin(),
                                second_tick.end(),
                                [ambient_index](const auto& p) {
                                    return p.emitter_index == ambient_index && p.age == 0.25f;
                                }),
                  4); // Existing motes keep ticking while new ones spawn.
    }
}

TEST_F(ParticleLiveTest, WaterfallOnlyLiveSetBecomesEmpty) {
    for (int site = 0; site < 2; ++site) {
        ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(20.0f), 8.0f, 4.0f),
                  ParticlePass::kInvalidEmitter);
    }
    pass.rebuild_emitter_descriptors(777u, 10u);
    pass.update(0.125f);
    ASSERT_GT(pass.live_particle_count(), 0u);
    const auto before = pass.live_particle_states();
    for (uint32_t index = 0; index < 2; ++index) {
        ASSERT_TRUE(std::any_of(before.begin(), before.end(), [index](const auto& particle) {
            return particle.emitter_index == index;
        }));
    }
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

TEST_F(ParticleLiveTest, ActiveTraversalMatchesSeededFullPoolBytesAndWork) {
    const Luminumbra::TestSupport::ScopedTempFile fixture("particle-reference-", ".json");
    for (const bool dense : {false, true}) {
        SCOPED_TRACE(dense);
        pass.clear_emitters();
        FullPoolReference reference;
        reference.wind = glm::vec3(0.75f, 0.0f, -0.5f);
        pass.set_wind(reference.wind);
        for (std::size_t index = 0; index < 3; ++index) {
            ParticlePass::EmitterData data;
            data.spawn_rate = index == 2   ? 0.0f
                              : dense      ? 262144.0f
                              : index == 0 ? 4096.0f
                                           : 3072.0f;
            data.lifetime = index == 0 ? 0.5f : 0.25f;
            data.origin = glm::vec3(1.0f, index == 1 ? 0.125f : 2.0f, -3.0f);
            data.origin_extent = glm::vec3(0.5f, 0.25f, 0.75f);
            data.base_velocity = glm::vec3(0.125f, index == 1 ? -2.0f : 1.0f, -0.25f);
            data.velocity_jitter = 0.5f;
            data.wind_response = 0.75f;
            data.streak_aspect = index == 1 ? 8.0f : 1.0f;
            data.impact_splash = index == 1;
            data.atlas_layer = static_cast<uint16_t>(index + 1);
            data.size_curve.points = {0.125f, 0.25f, 0.5f, 0.25f, 0.0f};
            data.r_curve.points = {0.25f, 0.5f, 0.75f, 1.0f, 0.5f};
            data.g_curve.points = {0.5f, 1.0f, 0.75f, 0.5f, 0.25f};
            data.b_curve.points = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
            data.a_curve.points = {0.0f, 0.5f, 1.0f, 0.5f, 0.0f};
            ASSERT_NO_FATAL_FAILURE(WriteEmitter(fixture.Path(), data));
            const auto id = index == 2 ? pass.add_splash_emitter(fixture.Path())
                                       : pass.add_emitter(fixture.Path(), glm::vec3(0.0f));
            ASSERT_EQ(id, index);
            reference.emitters.push_back({.data = data,
                                          .rng = ParticlePass::derive_emitter_seed(777u, 10u, id),
                                          .credit = 0.0});
        }
        pass.rebuild_emitter_descriptors(777u, 10u);
        const int frames = dense ? 4 : 160;
        for (int frame = 0; frame < frames; ++frame) {
            SCOPED_TRACE(frame);
            // Zero dt also includes deferred splash particles without drawing RNG.
            const float dt = frame % 7 == 6 ? 0.0f : 0.125f;
            reference.Update(dt);
            pass.update(dt);
            ASSERT_EQ(pass.frame_instance_count(), reference.records.size());
            EXPECT_EQ(pass.live_particle_count(), reference.records.size());
            if (!reference.records.empty()) {
                EXPECT_EQ(
                    std::memcmp(Mapping().records.data(),
                                reference.records.data(),
                                reference.records.size() * sizeof(ParticlePass::InstanceRecord)),
                    0);
            }
            ASSERT_NO_FATAL_FAILURE(ExpectStateBits(pass, reference));
            EXPECT_EQ(reference.visits, ParticlePass::kMaxInstances);
            EXPECT_EQ(pass.update_slot_visits(), reference.participating);
            if (!dense) {
                EXPECT_LT(pass.update_slot_visits(), ParticlePass::kMaxInstances / 16);
            }
        }
        EXPECT_GT(reference.impact_count, 0u);
        EXPECT_GT(reference.splash_count, 0u);
    }
}

TEST_F(ParticleLiveTest, ActiveTraversalEmptyExpiryRemovalAndBufferReset) {
    pass.update(0.125f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    ASSERT_NE(pass.add_splash_emitter(FixtureEmitter()), ParticlePass::kInvalidEmitter);
    pass.update(0.125f); // An emitter with no live slots must do no slot work.
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.clear_emitters();
    ASSERT_NE(pass.add_emitter(FixtureEmitter(), glm::vec3(0.0f)), ParticlePass::kInvalidEmitter);
    pass.rebuild_emitter_descriptors(777u, 10u);
    pass.update(0.125f);
    EXPECT_EQ(pass.update_slot_visits(), 15u);
    pass.update(3.0f); // Every existing and newly spawned particle expires.
    EXPECT_EQ(pass.frame_instance_count(), 0u);
    EXPECT_EQ(pass.live_particle_count(), 0u);
    pass.update(0.0f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.clear_emitters();
    ASSERT_NE(pass.add_waterfall_spray(spray_file, glm::vec3(0.0f), 8.0f, 4.0f),
              ParticlePass::kInvalidEmitter);
    pass.rebuild_emitter_descriptors(777u, 10u);
    pass.update(0.125f);
    ASSERT_GT(pass.update_slot_visits(), 0u);
    pass.clear_waterfall_spray();
    ASSERT_NE(pass.add_splash_emitter(FixtureEmitter()), ParticlePass::kInvalidEmitter);
    pass.update(0.0f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.clear_emitters();
    ASSERT_NE(pass.add_emitter(FixtureEmitter(), glm::vec3(0.0f)), ParticlePass::kInvalidEmitter);
    pass.update(0.125f);
    ASSERT_GT(pass.update_slot_visits(), 0u);
    pass.destroy_buffers();
    pass.update(0.125f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.init_buffers();
    pass.update(0.0f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.update(0.125f);
    EXPECT_EQ(pass.update_slot_visits(), 15u);
    pass.clear_emitters();
    ASSERT_NE(pass.add_splash_emitter(FixtureEmitter()), ParticlePass::kInvalidEmitter);
    pass.update(0.0f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    pass.clear_emitters();
    const Luminumbra::TestSupport::ScopedTempFile fixture("particle-capacity-", ".json");
    ParticlePass::EmitterData data;
    data.spawn_rate = static_cast<float>(ParticlePass::kMaxInstances) * 8.0f;
    ASSERT_NO_FATAL_FAILURE(WriteEmitter(fixture.Path(), data));
    ASSERT_NE(pass.add_emitter(fixture.Path(), glm::vec3(0.0f)), ParticlePass::kInvalidEmitter);
    pass.update(0.125f);
    EXPECT_EQ(pass.update_slot_visits(), ParticlePass::kMaxInstances);
    EXPECT_EQ(pass.frame_instance_count(), ParticlePass::kMaxInstances);
    pass.update(0.0f); // Includes the highest slot and the capacity sentinel.
    EXPECT_EQ(pass.update_slot_visits(), ParticlePass::kMaxInstances);
    pass.destroy_buffers();
    {
        ScopedParticleGlCall<PFNGLMAPBUFFERRANGEPROC> unmapped{glad_glMapBufferRange};
        pass.init_buffers();
    }
    pass.update(0.125f); // Spawns remain live when the mapping is unavailable.
    EXPECT_EQ(pass.update_slot_visits(), 0u);
    EXPECT_EQ(pass.frame_instance_count(), 0u);
    EXPECT_EQ(pass.live_particle_count(), ParticlePass::kMaxInstances);
    pass.destroy_buffers();
    pass.init_buffers();
    pass.update(0.0f);
    EXPECT_EQ(pass.update_slot_visits(), 0u);
}
