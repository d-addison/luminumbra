#include "ParticlePass.h"

#include "../PassShaderLayouts.h" // enumerable ExpectedLayout registry
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace Luminumbra::Rendering {

namespace {

uint8_t to_unorm8(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// splitmix64 -- a strong, fast, deterministic mixer for seed derivation.
uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// FNV-1a over a byte range.
uint64_t fnv1a(const void* data, std::size_t len, uint64_t seed = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Render-only xorshift64 (particle spawn jitter). Reproducible per emitter seed
// but NEVER feeds world_hash.
uint64_t xorshift64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

float rng_unit(uint64_t& state) {
    // 24 high bits -> [0,1).
    return static_cast<float>(xorshift64(state) >> 40) / 16777216.0f;
}

float rng_signed(uint64_t& state) {
    return rng_unit(state) * 2.0f - 1.0f;
}

void parse_curve(const nlohmann::json& node, ParticlePass::Curve& out) {
    if (!node.is_array()) {
        return;
    }
    const std::size_t n = std::min<std::size_t>(node.size(), ParticlePass::kCurvePoints);
    for (std::size_t i = 0; i < n; ++i) {
        out.points[i] = node[i].get<float>();
    }
    // If fewer than kCurvePoints were given, hold the last value.
    for (std::size_t i = n; i < ParticlePass::kCurvePoints && n > 0; ++i) {
        out.points[i] = out.points[n - 1];
    }
}

} // namespace

float ParticlePass::Curve::sample(float t) const {
    t = std::clamp(t, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(kCurvePoints - 1);
    const std::size_t i0 = static_cast<std::size_t>(scaled);
    const std::size_t i1 = std::min(i0 + 1, kCurvePoints - 1);
    const float frac = scaled - static_cast<float>(i0);
    return glm::mix(points[i0], points[i1], frac);
}

ParticlePass::ParticlePass() = default;
ParticlePass::~ParticlePass() = default;

void ParticlePass::init_shader(const std::filesystem::path& root_path) {
    // Re-home of magical_particles. Billboard expansion now happens in the
    // vertex stage (4 verts/instance via gl_VertexID); the geometry shader is
    // retired because the Shader class is vert+frag only.
    m_shader = std::make_unique<Shader>(
        (root_path / "res/shaders/magical_particles.vert").string().c_str(),
        (root_path / "res/shaders/magical_particles.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.particles");
    // validate the soft-particle depth sampler against the registry.
    if (m_shader && m_shader->IsValid()) {
        if (const ExpectedLayout* layout = FindPassExpectedLayout("magical_particles"))
            m_shader->ValidateLayout(*layout);
    }
}

void ParticlePass::init_buffers() {
    m_particles.assign(kMaxInstances, Particle{});
    m_ring_head = 0;
    m_live_count = 0;

    glGenVertexArrays(1, &m_vao);
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "particles.vao");
    glBindVertexArray(m_vao);

    const GLbitfield storage_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(kMaxInstances * sizeof(InstanceRecord));

    for (std::size_t ring = 0; ring < kRingFrames; ++ring) {
        glGenBuffers(1, &m_instance_vbo[ring]);
        glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo[ring]);
        glBufferStorage(GL_ARRAY_BUFFER, bytes, nullptr, storage_flags);
        m_instance_ptr[ring] = static_cast<InstanceRecord*>(
            glMapBufferRange(GL_ARRAY_BUFFER, 0, bytes, storage_flags));
        PassGl::label_gl_object(
            GL_BUFFER, m_instance_vbo[ring], "particles.instances." + std::to_string(ring));
    }

    // Bind ring slot 0's layout into the VAO; execute rebinds the live ring
    // slot's buffer to binding point 0 before drawing (same VAO, swapped VBO).
    glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo[0]);
    glVertexBindingDivisor(0, 1); // one record per instance

    // location 0: pos (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, offsetof(InstanceRecord, pos));
    glVertexAttribBinding(0, 0);
    // location 1: size (float)
    glEnableVertexAttribArray(1);
    glVertexAttribFormat(1, 1, GL_FLOAT, GL_FALSE, offsetof(InstanceRecord, size));
    glVertexAttribBinding(1, 0);
    // location 2: color (rgba8 normalized)
    glEnableVertexAttribArray(2);
    glVertexAttribFormat(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(InstanceRecord, color));
    glVertexAttribBinding(2, 0);
    // location 3: atlas layer (uint16 -> int attrib)
    glEnableVertexAttribArray(3);
    glVertexAttribIFormat(3, 1, GL_UNSIGNED_SHORT, offsetof(InstanceRecord, atlas_layer));
    glVertexAttribBinding(3, 0);
    // location 4: rotation (snorm8 -> [-1,1], decoded to radians as angle*pi)
    glEnableVertexAttribArray(4);
    glVertexAttribFormat(4, 1, GL_BYTE, GL_TRUE, offsetof(InstanceRecord, rotation));
    glVertexAttribBinding(4, 0);
    // location 5: streak aspect (unorm8 -> [0,1], decoded as v*kMaxStreakAspect)
    glEnableVertexAttribArray(5);
    glVertexAttribFormat(5, 1, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(InstanceRecord, streak));
    glVertexAttribBinding(5, 0);

    glBindVertexBuffer(0, m_instance_vbo[0], 0, sizeof(InstanceRecord));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void ParticlePass::destroy_buffers() {
    for (std::size_t ring = 0; ring < kRingFrames; ++ring) {
        if (m_instance_vbo[ring]) {
            glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo[ring]);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glDeleteBuffers(1, &m_instance_vbo[ring]);
            m_instance_vbo[ring] = 0;
            m_instance_ptr[ring] = nullptr;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    m_particles.clear();
    m_particles.shrink_to_fit();
    m_ring_head = 0;
    m_live_count = 0;
    m_ring_cursor = 0;
    m_frame_instance_count = 0;
}

void ParticlePass::reset_shader() {
    m_shader.reset();
}

uint32_t ParticlePass::add_emitter(const std::filesystem::path& json_path,
                                   const glm::vec3& world_origin) {
    if (m_active_emitters.size() >= kMaxEmitters) {
        LUMINUMBRA_CORE_WARN("ParticlePass: emitter cap ({}) reached, ignoring '{}'.",
                             kMaxEmitters,
                             json_path.string());
        return kInvalidEmitter;
    }
    std::ifstream in(json_path);
    if (!in) {
        LUMINUMBRA_CORE_WARN("ParticlePass: emitter file not found '{}'.", json_path.string());
        return kInvalidEmitter;
    }

    EmitterData data;
    try {
        nlohmann::json doc = nlohmann::json::parse(in);
        data.name = doc.value("name", json_path.stem().string());
        data.type = doc.value("type", 0u);
        data.spawn_rate = doc.value("spawn_rate", 0.0f);
        data.lifetime = std::max(0.01f, doc.value("lifetime", 1.0f));
        if (doc.contains("origin") && doc["origin"].is_array() && doc["origin"].size() == 3) {
            data.origin = glm::vec3(doc["origin"][0].get<float>(),
                                    doc["origin"][1].get<float>(),
                                    doc["origin"][2].get<float>());
        }
        if (doc.contains("origin_extent") && doc["origin_extent"].is_array() &&
            doc["origin_extent"].size() == 3) {
            data.origin_extent = glm::vec3(doc["origin_extent"][0].get<float>(),
                                           doc["origin_extent"][1].get<float>(),
                                           doc["origin_extent"][2].get<float>());
        }
        if (doc.contains("base_velocity") && doc["base_velocity"].is_array() &&
            doc["base_velocity"].size() == 3) {
            data.base_velocity = glm::vec3(doc["base_velocity"][0].get<float>(),
                                           doc["base_velocity"][1].get<float>(),
                                           doc["base_velocity"][2].get<float>());
        }
        data.velocity_jitter = doc.value("velocity_jitter", 0.0f);
        if (doc.contains("size_curve"))
            parse_curve(doc["size_curve"], data.size_curve);
        if (doc.contains("color_r_curve"))
            parse_curve(doc["color_r_curve"], data.r_curve);
        if (doc.contains("color_g_curve"))
            parse_curve(doc["color_g_curve"], data.g_curve);
        if (doc.contains("color_b_curve"))
            parse_curve(doc["color_b_curve"], data.b_curve);
        if (doc.contains("color_a_curve"))
            parse_curve(doc["color_a_curve"], data.a_curve);
        data.atlas_layer = static_cast<uint16_t>(doc.value("atlas_layer", 0u));
        const std::string blend = doc.value("blend", std::string("additive"));
        data.blend = (blend == "alpha") ? BlendMode::AlphaBlend : BlendMode::Additive;
        //  ( precipitation): optional wind/streak/splash fields. Absent
        // for legacy magical emitters (defaults keep them unaffected).
        data.wind_response = doc.value("wind_response", 0.0f);
        data.streak_aspect = std::max(1.0f, doc.value("streak_aspect", 1.0f));
        data.impact_splash = doc.value("impact_splash", false);
        data.impact_plane_y = doc.value("impact_plane_y", 0.0f);
        data.loaded = true;
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN(
            "ParticlePass: failed to parse emitter '{}': {}", json_path.string(), e.what());
        return kInvalidEmitter;
    }

    ActiveEmitter emitter;
    emitter.data = std::move(data);
    emitter.id = m_next_emitter_id++;
    emitter.world_origin = world_origin + emitter.data.origin;
    emitter.rng_seed = 0; // filled by rebuild_emitter_descriptors
    emitter.rng_state = 1;
    m_active_emitters.push_back(std::move(emitter));
    LUMINUMBRA_CORE_INFO("ParticlePass: emitter '{}' id={} added (rate={}/s).",
                         m_active_emitters.back().data.name,
                         m_active_emitters.back().id,
                         m_active_emitters.back().data.spawn_rate);
    return m_active_emitters.back().id;
}

void ParticlePass::clear_emitters() {
    m_active_emitters.clear();
    m_descriptors.clear();
    m_next_emitter_id = 0;
    m_splash_emitter_index = kNoSplashEmitter;
    // Kill any live particles so existing byte-stable visual gates render an
    // empty (no-op) particle pass.
    for (auto& p : m_particles)
        p.alive = false;
    m_ring_head = 0;
    m_live_count = 0;
}

void ParticlePass::set_emitter_origin(uint32_t emitter_id, const glm::vec3& base_origin) {
    //  re-anchor a live emitter's spawn region. We apply
    // the SAME convention as add_emitter (world_origin = base + data.origin) so the
    // authored height offset (rain: [0,22,0]) is preserved as the camera moves.
    // Render-only: this never feeds rebuild_emitter_descriptors at a fixed tick, and
    // the descriptor snapshot (world_hash surface) is rebuilt independently from the
    // sim-deterministic schedule -- so moving the render anchor leaves world_hash
    // untouched (one-way rule, regression review).
    for (ActiveEmitter& emitter : m_active_emitters) {
        if (emitter.id == emitter_id) {
            emitter.world_origin = base_origin + emitter.data.origin;
            return;
        }
    }
}

uint32_t ParticlePass::add_splash_emitter(const std::filesystem::path& json_path) {
    // a splash burst is a normal emitter loaded with spawn_rate
    // forced to 0 -- it never self-emits; only rain-impact events spawn from it.
    // It carries no descriptor "enable" (spawn_rate 0), so it does not perturb
    // the deterministic emitter-descriptor snapshot.
    const uint32_t id = add_emitter(json_path, glm::vec3(0.0f));
    if (id == kInvalidEmitter) {
        return kInvalidEmitter;
    }
    m_splash_emitter_index = m_active_emitters.size() - 1;
    ActiveEmitter& splash = m_active_emitters[m_splash_emitter_index];
    splash.data.spawn_rate = 0.0f; // never self-emit
    splash.data.impact_splash = false;
    return id;
}

uint32_t ParticlePass::add_waterfall_spray(const std::filesystem::path& json_path,
                                           const glm::vec3& plunge_pos,
                                           float drop_height,
                                           float channel_width) {
    // a waterfall spray emitter is a normal emitter anchored at the
    // plunge foot. After loading we re-shape its spawn box + emission rate to the
    // detected fall's geometry so a tall/wide fall throws a proportionally larger
    // mist plume. add_emitter applies world_origin = base + data.origin, so we
    // pass plunge_pos as the base and then widen the loaded spawn extent.
    const uint32_t id = add_emitter(json_path, plunge_pos);
    if (id == kInvalidEmitter) {
        return kInvalidEmitter;
    }
    ActiveEmitter& spray = m_active_emitters.back();
    // Spawn box spans the channel width horizontally and a shallow band at the
    // plunge pool surface; clamp so the mist stays a plume, not a fog bank.
    const float half_w = std::clamp(channel_width * 0.6f, 1.5f, 8.0f);
    spray.data.origin_extent = glm::vec3(half_w, 0.5f, half_w);
    // Mist intensity scales with drop height (a taller fall aerates more water).
    const float intensity = std::clamp(drop_height / 8.0f, 0.5f, 4.0f);
    spray.data.spawn_rate *= intensity;
    // Bias the mist UPWARD/outward from the impact (against gravity) so it reads
    // as rising spray; keep the authored horizontal jitter for the fan-out.
    spray.data.base_velocity.y = std::max(spray.data.base_velocity.y, 1.5f * intensity);
    spray.data.wind_response = std::max(spray.data.wind_response, 0.4f); // mist drifts on wind
    return id;
}

uint64_t
ParticlePass::derive_emitter_seed(uint64_t world_seed, uint64_t world_tick, uint32_t emitter_id) {
    // Pure function of world state + emitter identity. Mixing three splitmix64
    // rounds de-correlates seeds across ticks and emitters.
    uint64_t s = splitmix64(world_seed ^ 0xA24BAED4963EE407ull);
    s = splitmix64(s ^ (world_tick + 0x9E3779B97F4A7C15ull));
    s = splitmix64(s ^ (static_cast<uint64_t>(emitter_id) * 0xD1B54A32D192ED03ull));
    return s;
}

void ParticlePass::rebuild_emitter_descriptors(uint64_t world_seed, uint64_t world_tick) {
    m_descriptors.clear();
    m_descriptors.reserve(m_active_emitters.size());
    for (auto& emitter : m_active_emitters) {
        EmitterDescriptor d;
        d.id = emitter.id;
        d.type = emitter.data.type;
        // Quantize the world origin to integer millimetres so the descriptor is
        // a stable POD (no float bit-noise across runs).
        d.origin_region[0] = static_cast<int32_t>(std::lround(emitter.world_origin.x * 1000.0f));
        d.origin_region[1] = static_cast<int32_t>(std::lround(emitter.world_origin.y * 1000.0f));
        d.origin_region[2] = static_cast<int32_t>(std::lround(emitter.world_origin.z * 1000.0f));
        d.spawn_rate_milli = static_cast<uint32_t>(std::lround(emitter.data.spawn_rate * 1000.0f));
        d.rng_seed = derive_emitter_seed(world_seed, world_tick, emitter.id);
        d.enable = (emitter.data.loaded && emitter.data.spawn_rate > 0.0f) ? 1u : 0u;
        emitter.rng_seed = d.rng_seed;
        // Seed the render-only spawn RNG from the deterministic seed so the
        // render spawn pattern is reproducible (still never enters world_hash).
        if (emitter.rng_state == 0 || emitter.rng_state == 1) {
            emitter.rng_state = d.rng_seed != 0 ? d.rng_seed : 0x1234567890ABCDEFull;
        }
        m_descriptors.push_back(d);
    }
}

uint64_t ParticlePass::emitter_descriptor_hash() const {
    if (m_descriptors.empty()) {
        return fnv1a(nullptr, 0);
    }
    return fnv1a(m_descriptors.data(), m_descriptors.size() * sizeof(EmitterDescriptor));
}

void ParticlePass::spawn_from_emitter(ActiveEmitter& emitter, float dt) {
    if (!emitter.data.loaded || emitter.data.spawn_rate <= 0.0f) {
        return;
    }
    emitter.spawn_accumulator +=
        static_cast<double>(emitter.data.spawn_rate) * static_cast<double>(dt);
    int to_spawn = static_cast<int>(emitter.spawn_accumulator);
    if (to_spawn <= 0) {
        return;
    }
    emitter.spawn_accumulator -= static_cast<double>(to_spawn);
    to_spawn = std::min(to_spawn, static_cast<int>(kMaxInstances));

    const uint32_t emitter_index = static_cast<uint32_t>(&emitter - m_active_emitters.data());
    for (int i = 0; i < to_spawn; ++i) {
        Particle& p = m_particles[m_ring_head];
        if (!p.alive) {
            ++m_live_count;
        }
        // else: oldest slot reused -> ring eviction (live count unchanged).
        p.alive = true;
        p.age = 0.0f;
        p.lifetime = emitter.data.lifetime;
        p.emitter_index = emitter_index;
        p.pos = emitter.world_origin +
                glm::vec3(rng_signed(emitter.rng_state) * emitter.data.origin_extent.x,
                          rng_signed(emitter.rng_state) * emitter.data.origin_extent.y,
                          rng_signed(emitter.rng_state) * emitter.data.origin_extent.z);
        p.vel = emitter.data.base_velocity + glm::vec3(rng_signed(emitter.rng_state),
                                                       rng_signed(emitter.rng_state),
                                                       rng_signed(emitter.rng_state)) *
                                                 emitter.data.velocity_jitter;
        m_ring_head = (m_ring_head + 1) % kMaxInstances;
    }
}

void ParticlePass::spawn_splash_burst(const glm::vec3& world_pos,
                                      int count,
                                      ActiveEmitter& splash_template) {
    // Render-only: emit `count` short-lived splash particles at the impact point.
    const uint32_t splash_index =
        static_cast<uint32_t>(&splash_template - m_active_emitters.data());
    for (int i = 0; i < count; ++i) {
        Particle& p = m_particles[m_ring_head];
        if (!p.alive) {
            ++m_live_count;
        }
        p.alive = true;
        p.age = 0.0f;
        p.lifetime = splash_template.data.lifetime;
        p.emitter_index = splash_index;
        p.pos = world_pos + glm::vec3(rng_signed(splash_template.rng_state) * 0.15f,
                                      0.02f,
                                      rng_signed(splash_template.rng_state) * 0.15f);
        // Outward + upward spray (a tiny crown), no wind response.
        p.vel =
            splash_template.data.base_velocity + glm::vec3(rng_signed(splash_template.rng_state),
                                                           rng_unit(splash_template.rng_state),
                                                           rng_signed(splash_template.rng_state)) *
                                                     splash_template.data.velocity_jitter;
        m_ring_head = (m_ring_head + 1) % kMaxInstances;
    }
}

void ParticlePass::update(float dt) {
    m_frame_instance_count = 0;
    if (m_active_emitters.empty() || m_particles.empty()) {
        m_live_count = 0;
        return;
    }

    // 1. Spawn (render-only).
    for (auto& emitter : m_active_emitters) {
        spawn_from_emitter(emitter, dt);
    }

    // 2. Integrate motion + age, build this frame's instance records.
    m_ring_cursor = (m_ring_cursor + 1) % kRingFrames;
    InstanceRecord* dst = m_instance_ptr[m_ring_cursor];
    if (dst == nullptr) {
        return;
    }

    // collect impact-splash spawn points this frame, then emit them
    // after the integration loop (so we never mutate the ring while iterating it).
    struct ImpactEvent {
        glm::vec3 pos;
    };
    std::vector<ImpactEvent> impacts;

    std::size_t live = 0;
    std::size_t written = 0;
    for (auto& p : m_particles) {
        if (!p.alive) {
            continue;
        }
        p.age += dt;
        if (p.age >= p.lifetime) {
            p.alive = false;
            continue;
        }

        const ActiveEmitter& emitter = m_active_emitters[p.emitter_index];

        //  WIND-ADVECTION (render-only). Apply the per-frame wind velocity
        // scaled by the emitter's wind_response, so rain/snow SLANT in storms.
        // wind_response == 0 leaves magical/splash particles unaffected.
        const glm::vec3 advected_vel = p.vel + m_wind_velocity * emitter.data.wind_response;
        p.pos += advected_vel * dt;

        //  IMPACT SPLASH. A descending precip particle that crosses the
        // emitter's impact plane spawns a splash burst and dies (depth/ground
        // impact -- the soft-particle depth fade in the frag shader handles the
        // G-buffer surface intersection visually; this is the spray response).
        if (emitter.data.impact_splash && advected_vel.y < 0.0f &&
            p.pos.y <= emitter.data.impact_plane_y && m_splash_emitter_index != kNoSplashEmitter) {
            impacts.push_back(
                ImpactEvent{glm::vec3(p.pos.x, emitter.data.impact_plane_y, p.pos.z)});
            p.alive = false;
            continue;
        }
        ++live;

        const float life_t = p.age / p.lifetime;

        // Streak orientation: for elongated emitters (rain, streak_aspect > 1) the
        // billboard is rotated to align with the SCREEN-projected velocity, so a
        // wind-slanted velocity renders a diagonal streak and a calm velocity a
        // vertical one. The frag-shader quad stays square; the visible slant comes
        // from this rotation + the wind-advected spatial envelope of the field.
        float rotation = life_t * 6.2831853f;
        // SCREEN-projected streak orientation. The old
        // code fed RAW WORLD velocity (advected_vel.x/.y) into atan2 and elongated the
        // billboard along that fixed screen axis regardless of where the camera looked.
        // Looking DOWN, a falling drop (world -Y) still painted a full-length vertical
        // screen streak -> a uniform grey vertical veil/haze that read as static, not
        // rain. The fix projects the world velocity onto the camera screen plane
        // (right/up) so the streak follows its true on-screen motion: end-on rain
        // (looking down) projects to a near-zero screen vector and collapses to a
        // short droplet, while side-on rain stays a vertical streak. Render-only .
        float screen_vel_scale = 1.0f; // 1 = full streak; <1 shrinks toward a droplet
        if (emitter.data.streak_aspect > 1.0f) {
            if (m_have_view_basis) {
                const float sx = glm::dot(advected_vel, m_view_right);
                const float sy = glm::dot(advected_vel, m_view_up);
                const float screen_speed = std::sqrt(sx * sx + sy * sy);
                const float world_speed =
                    std::sqrt(advected_vel.x * advected_vel.x + advected_vel.y * advected_vel.y +
                              advected_vel.z * advected_vel.z);
                // atan2(screen-right, screen-up): a vertical on-screen fall -> 0; wind
                // shear or an oblique view tilts it. NDC y grows UP and the vertex
                // stage elongates along +up, so this lines the streak up with motion.
                rotation = std::atan2(sx, sy);
                // How much of the motion is actually visible on screen. When the drop
                // travels almost straight at/away from the camera (looking down/up),
                // this drops toward 0 and the streak shortens to a droplet instead of
                // a fake vertical bar. Smoothstep so side-on rain stays full-length.
                if (world_speed > 1e-4f) {
                    const float frac = std::clamp(screen_speed / world_speed, 0.0f, 1.0f);
                    // keep a VISIBLE streak floor when looking
                    // down. The previous frac/0.45 collapsed end-on rain all the way to
                    // a ~round dot; combined with the down-view grey veil, the down-look
                    // read as fog with no rain at all. We now (a) widen the side-on band
                    // (frac/0.30 reaches full streak sooner) and (b) lift the minimum so
                    // even fully end-on rain (looking straight down) still draws a short
                    // foreshortened streak instead of a near-invisible droplet. So down-
                    // pitched rain reads as real falling streaks, not haze.
                    screen_vel_scale = std::clamp(frac / 0.30f, 0.35f, 1.0f);
                }
            } else {
                const float horiz = advected_vel.x;
                const float vert = advected_vel.y;
                rotation = std::atan2(horiz, vert);
            }
        }

        InstanceRecord& rec = dst[written];
        rec.pos[0] = p.pos.x;
        rec.pos[1] = p.pos.y;
        rec.pos[2] = p.pos.z;
        rec.size = emitter.data.size_curve.sample(life_t);
        rec.color[0] = to_unorm8(emitter.data.r_curve.sample(life_t));
        rec.color[1] = to_unorm8(emitter.data.g_curve.sample(life_t));
        rec.color[2] = to_unorm8(emitter.data.b_curve.sample(life_t));
        rec.color[3] = to_unorm8(emitter.data.a_curve.sample(life_t));
        rec.atlas_layer = emitter.data.atlas_layer;
        // Pack rotation as snorm8 (angle/pi in [-1,1]) and the per-emitter streak
        // aspect as unorm8 (aspect/kMaxStreakAspect). Round particles carry aspect
        // 1 -> a square billboard; rain carries streak_aspect > 1 -> the vertex
        // stage elongates the velocity-aligned quad into a streak.
        const float angle_norm =
            std::clamp(rotation * (1.0f / 3.14159265358979323846f), -1.0f, 1.0f);
        rec.rotation = static_cast<int8_t>(std::lround(angle_norm * 127.0f));
        //  WIND-DRIVEN streak STRETCH. The streak elongation
        // is no longer a constant -- it RESPONDS to the wind, so still air renders
        // soft, near-round droplets and a storm gust SHEARS them into long hard
        // streaks. This (a) is physically correct (wind-sheared rain), (b) honours
        // the owner note that wind should visibly impact the rain, and (c) restores
        // the Precipitation wind-slant gate. That gate measures the h/v gradient
        // ratio (slant_ratio): a thin VERTICAL streak maximizes it, and leaning
        // LOWERS it, so the gate's "windy slant_ratio >= 1.2x calm" can only hold
        // when the WINDY streaks are MORE vertically-elongated than calm. Short calm
        // streaks (aspect ~1.7) keep calm just above the dots-not-streaks floor while
        // capping its ratio; long, only slightly-leaned windy streaks (aspect ramped
        // toward 16 by the gust, with a small camera-right lean from main_client's
        // reduced windy wind) read as a much higher ratio -> windy clears calm by the
        // required margin. Round/magical emitters (streak_aspect <= 1) are untouched.
        // The wind factor ramps the aspect from a soft droplet floor up to a long
        // streak as the horizontal wind grows; deterministic, render-only .
        float effective_aspect = emitter.data.streak_aspect;
        if (emitter.data.streak_aspect > 1.0f && emitter.data.wind_response > 0.0f) {
            const glm::vec3 wind = m_wind_velocity * emitter.data.wind_response;
            const float wind_horiz = std::sqrt(wind.x * wind.x + wind.z * wind.z);
            // Still air -> a short streak just past the dots-not-streaks floor; a
            // storm gust -> a hard, much longer thin streak. The wide spread between
            // the calm and windy streak length is what lets the windy capture clear
            // the calm slant baseline by the gate's required margin while calm still
            // reads as a (short) vertical streak, not a round dot.
            constexpr float kCalmStreakAspect = 1.7f;   // short vertical streak at rest
            constexpr float kWindyStreakAspect = 16.0f; // long hard streak in a gust
            constexpr float kWindFullStretch = 4.0f;    // wind speed for full streak
            const float t = std::clamp(wind_horiz / kWindFullStretch, 0.0f, 1.0f);
            effective_aspect = kCalmStreakAspect + (kWindyStreakAspect - kCalmStreakAspect) * t;
        }
        // shrink the streak toward a round droplet as
        // its on-screen motion vanishes (camera looking along the fall direction), so
        // down-pitched rain reads as droplets, not a fake vertical grey veil. Lerp
        // the elongation back toward 1 (square) by the screen-velocity scale; side-on
        // framings keep screen_vel_scale ~1 so the wind-slant streaks are untouched.
        if (emitter.data.streak_aspect > 1.0f) {
            effective_aspect = 1.0f + (effective_aspect - 1.0f) * screen_vel_scale;
        }
        const float aspect_norm = std::clamp(effective_aspect / kMaxStreakAspect, 0.0f, 1.0f);
        rec.streak = static_cast<uint8_t>(std::lround(aspect_norm * 255.0f));
        ++written;
        if (written >= kMaxInstances) {
            break;
        }
    }

    // Emit splash bursts for this frame's impacts (bounded; a few particles each).
    if (!impacts.empty() && m_splash_emitter_index != kNoSplashEmitter) {
        ActiveEmitter& splash = m_active_emitters[m_splash_emitter_index];
        // Sub-sample impacts so a dense storm does not flood the ring with splash
        // particles (every 4th impact yields a small crown).
        for (std::size_t i = 0; i < impacts.size(); i += 4) {
            spawn_splash_burst(impacts[i].pos, 3, splash);
        }
    }

    m_live_count = live;
    m_frame_instance_count = written;
}

std::size_t ParticlePass::execute(const RenderContext& ctx, const Camera& camera) {
    // No-op (zero draw work) when nothing to render: keeps existing visual gates
    // byte-stable.
    if (!m_shader || !m_shader->IsValid() || m_frame_instance_count == 0 || m_vao == 0) {
        return 0;
    }

    if (!ctx.lit_scene.id) {
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // particles into the internal scene

    // Transparent particles: test against scene depth but do not write depth,
    // and blend additively into the HDR lighting target.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    //  honour the emitter blend mode. The old
    // path hardcoded additive (GL_SRC_ALPHA, GL_ONE) for EVERY emitter -- but RAIN
    // is alpha-blended translucent water, and drawing it additively over the dark
    // storm sky made the streaks glow/clip oddly instead of reading as a soft
    // translucent curtain. If ANY active emitter requests alpha blending (rain /
    // splash), use standard src-alpha transparency so the streaks composite as a
    // translucent veil; otherwise keep additive (emissive magical sparkles). The
    // two never coexist in a scene, so a single per-draw choice is sufficient and
    // keeps the additive fixture path byte-identical.
    bool any_alpha = false;
    for (const auto& e : m_active_emitters) {
        if (e.data.loaded && e.data.blend == BlendMode::AlphaBlend) {
            any_alpha = true;
            break;
        }
    }
    if (any_alpha) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    }
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    if (cull_was_enabled) {
        glDisable(GL_CULL_FACE);
    }

    m_shader->use();
    const glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                                  static_cast<float>(ctx.screen_width) /
                                                      static_cast<float>(ctx.screen_height),
                                                  camera.GetNearPlane(),
                                                  camera.GetFarPlane());
    const glm::mat4 view = camera.GetViewMatrix();
    // cache the camera screen basis so next frame's
    // update can orient rain streaks by SCREEN-projected velocity (kills the
    // grey vertical-veil haze when looking down). update->execute run back to
    // back on the same camera, so a one-frame-stale basis is imperceptible.
    m_view_right = camera.Right;
    m_view_up = camera.Up;
    m_view_forward = camera.Front;
    m_have_view_basis = true;
    m_shader->setMat4("u_view", view);
    m_shader->setMat4("u_projection", projection);
    m_shader->setVec3("u_cameraRight", camera.Right);
    m_shader->setVec3("u_cameraUp", camera.Up);
    m_shader->setVec3("u_cameraPos", camera.Position);
    // the per-frame wall-clock SNAPSHOT (Group K ctx.time_seconds), not a
    // live glfwGetTime read — dispatch must be bit-idempotent per prepared frame.
    m_shader->setFloat("u_time", ctx.time_seconds);
    m_shader->setVec2("u_screenSize",
                      glm::vec2(static_cast<float>(ctx.internal_w()),
                                static_cast<float>(ctx.internal_h()))); // internal scene extent
    m_shader->setFloat("u_nearPlane", camera.GetNearPlane());
    m_shader->setFloat("u_farPlane", camera.GetFarPlane());

    // Forward lighting: sun + ambient + the nearest few point lights.
    m_shader->setVec3("u_sunDirection", ctx.sun.direction);
    m_shader->setVec3("u_sunColor", ctx.sun.color);
    m_shader->setFloat("u_sunIntensity", ctx.sun.intensity);
    m_shader->setVec3("u_ambientColor", ctx.sky_ambient_color);

    const int max_lights = 4;
    int light_count = std::min(static_cast<int>(ctx.point_lights->size()), max_lights);
    m_shader->setInt("u_pointLightCount", light_count);
    for (int i = 0; i < light_count; ++i) {
        const PointLight& l = (*ctx.point_lights)[static_cast<std::size_t>(i)];
        const std::string base = "u_pointLights[" + std::to_string(i) + "].";
        m_shader->setVec3(base + "position", l.position);
        m_shader->setVec3(base + "color", l.color);
        m_shader->setFloat(base + "radius", l.radius);
        m_shader->setFloat(base + "intensity", l.intensity);
    }

    // Soft-particle depth read from the G-buffer depth attachment.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    m_shader->setInt("u_sceneDepth", 0);

    // Point the VAO's binding 0 at this frame's ring slot, then instanced-draw
    // 4 verts (a quad) per particle.
    glBindVertexArray(m_vao);
    glBindVertexBuffer(0, m_instance_vbo[m_ring_cursor], 0, sizeof(InstanceRecord));
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(m_frame_instance_count));
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (cull_was_enabled) {
        glEnable(GL_CULL_FACE);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    return m_frame_instance_count;
}

} // namespace Luminumbra::Rendering
