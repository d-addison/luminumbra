#include "FoliagePass.h"

#include "GBufferPass.h"
#include "LightingPass.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include <iterator>
#include <limits>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace Luminumbra::Rendering {

namespace {

// the foliage async-readback ring slot layout is
// [u32 instanceCount][kMaxInstances blade records]. The count is copied from
// m_count_ssbo (DrawArraysIndirectCommand.instanceCount, at byte offset
// sizeof(GLuint)*2); the blades follow it.
constexpr std::size_t kReadbackCountBytes = sizeof(GLuint);

// splitmix64 -- the same deterministic mixer the  particle pass uses for seed
// derivation. PURE: no global RNG, no world-seed offset consumed.
uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// FNV-1a over a byte range (instance-hash determinism surface).
uint64_t fnv1a(const void* data, std::size_t len, uint64_t seed = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// 64 high bits of a splitmix64 word -> [0,1).
float hash_unit(uint64_t h) {
    return static_cast<float>(h >> 40) / 16777216.0f;
}

uint8_t to_unorm8(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// IEEE-754 half-precision encode (truncating; deterministic, adequate for an
// angle), matching ParticlePass::encode_f16.
uint16_t encode_f16(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    if (exponent >= 0x1F) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

} // namespace

FoliagePass::FoliagePass() = default;
FoliagePass::~FoliagePass() = default;

uint64_t
FoliagePass::placement_hash(int chunk_x, int chunk_z, u8 biome_id, uint32_t instance_index) {
    // PINNED placement function (documented design): a pure hash of
    // (chunk coords, biome id, instance index). Slope/moisture modulate the
    // EMIT decision downstream (not the hash) so the hash stays a stable
    // function of the world grid. NO global RNG, NO seed offset.
    uint64_t h =
        splitmix64(static_cast<uint64_t>(static_cast<uint32_t>(chunk_x)) ^ 0x51AF7C3D9E0B12A7ull);
    h = splitmix64(h ^
                   (static_cast<uint64_t>(static_cast<uint32_t>(chunk_z)) * 0xD1B54A32D192ED03ull));
    h = splitmix64(h ^ (static_cast<uint64_t>(biome_id) * 0x9E3779B97F4A7C15ull));
    h = splitmix64(h ^ (static_cast<uint64_t>(instance_index) * 0xA24BAED4963EE407ull));
    return h;
}

void FoliagePass::init_shader(const std::filesystem::path& root_path) {
    m_shader = std::make_unique<Shader>((root_path / "res/shaders/foliage.vert").string().c_str(),
                                        (root_path / "res/shaders/foliage.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shader ? m_shader->Id() : 0u, "shader.foliage");
}

void FoliagePass::init_buffers() {
    m_instances.clear();
    m_instances.reserve(4096);
    m_frame_instance_count = 0;

    glGenVertexArrays(1, &m_vao);
    PassGl::label_gl_object(GL_VERTEX_ARRAY, m_vao, "foliage.vao");
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
            GL_BUFFER, m_instance_vbo[ring], "foliage.instances." + std::to_string(ring));
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo[0]);
    glVertexBindingDivisor(0, 1); // one record per instance

    // location 0: pos (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, offsetof(InstanceRecord, pos));
    glVertexAttribBinding(0, 0);
    // location 1: size (vec2)
    glEnableVertexAttribArray(1);
    glVertexAttribFormat(1, 2, GL_FLOAT, GL_FALSE, offsetof(InstanceRecord, size));
    glVertexAttribBinding(1, 0);
    // location 2: color (rgba8 normalized)
    glEnableVertexAttribArray(2);
    glVertexAttribFormat(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(InstanceRecord, color));
    glVertexAttribBinding(2, 0);
    // location 3: sway (vec2)
    glEnableVertexAttribArray(3);
    glVertexAttribFormat(3, 2, GL_FLOAT, GL_FALSE, offsetof(InstanceRecord, sway));
    glVertexAttribBinding(3, 0);
    // location 4: phase (half float)
    glEnableVertexAttribArray(4);
    glVertexAttribFormat(4, 1, GL_HALF_FLOAT, GL_FALSE, offsetof(InstanceRecord, phase));
    glVertexAttribBinding(4, 0);
    // location 5: facing (half float)
    glEnableVertexAttribArray(5);
    glVertexAttribFormat(5, 1, GL_HALF_FLOAT, GL_FALSE, offsetof(InstanceRecord, facing));
    glVertexAttribBinding(5, 0);

    glBindVertexBuffer(0, m_instance_vbo[0], 0, sizeof(InstanceRecord));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void FoliagePass::destroy_buffers() {
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
    m_instances.clear();
    m_instances.shrink_to_fit();
    m_ring_cursor = 0;
    m_frame_instance_count = 0;
}

void FoliagePass::reset_shader() {
    m_shader.reset();
}

namespace {
// Minimal compute-program compile/link helper. Returns 0
// on any failure so the caller can fall back to the CPU scatter path.
GLuint compile_compute_program(const std::string& source) {
    const char* src = source.c_str();
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LUMINUMBRA_CORE_WARN("FoliagePass: grass_scatter.comp compile failed: {}", log);
        glDeleteShader(s);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(s);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LUMINUMBRA_CORE_WARN("FoliagePass: grass_scatter program link failed: {}", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}
} // namespace

void FoliagePass::init_compute(const std::filesystem::path& root_path) {
    // Load + compile the scatter compute shader. On any failure the pass keeps
    // m_gpu_scatter=false and the CPU rebuild loop is used (graceful fallback).
    std::ifstream in(root_path / "res/shaders/grass_scatter.comp");
    if (!in) {
        LUMINUMBRA_CORE_WARN("FoliagePass: grass_scatter.comp not found; using CPU scatter.");
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    m_compute_prog = compile_compute_program(ss.str());
    if (m_compute_prog == 0) {
        return; // CPU fallback
    }
    PassGl::label_gl_object(GL_PROGRAM, m_compute_prog, "shader.grass_scatter");

    glGenBuffers(1, &m_chunk_ssbo);
    glGenBuffers(1, &m_surf_ssbo);
    glGenBuffers(1, &m_blade_ssbo);
    glGenBuffers(1, &m_count_ssbo);
    glGenBuffers(1, &m_arch_ssbo);

    // The blade SSBO is sized for the full pool and is ALSO bound as the draw's
    // instance ARRAY_BUFFER in execute (same buffer, two targets).
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_blade_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(kMaxInstances * sizeof(InstanceRecord)),
                 nullptr,
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_count_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLuint) * 5, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    PassGl::label_gl_object(GL_BUFFER, m_blade_ssbo, "grass.blades.ssbo");
    PassGl::label_gl_object(GL_BUFFER, m_count_ssbo, "grass.count_and_draw.ssbo");

    m_gpu_scatter = true;
    LUMINUMBRA_CORE_INFO("FoliagePass: GPU grass scatter active (compute + SSBO).");
}

void FoliagePass::destroy_compute() {
    if (m_compute_prog) {
        glDeleteProgram(m_compute_prog);
        m_compute_prog = 0;
    }
    GLuint bufs[] = {m_chunk_ssbo, m_surf_ssbo, m_blade_ssbo, m_count_ssbo, m_arch_ssbo};
    for (GLuint& b : bufs) {
        if (b)
            glDeleteBuffers(1, &b);
    }
    m_chunk_ssbo = m_surf_ssbo = m_blade_ssbo = m_count_ssbo = m_arch_ssbo = 0;
    // Free the async-readback ring's GL slots HERE, under a live context, matching
    // the explicit-teardown convention of every other GL resource in this class
    // (so the ring's destructor shutdown is a safe no-op even if FoliagePass is
    // destroyed after the GL context is gone).
    m_readback_ring.shutdown();
    m_gpu_scatter = false;
    m_gpu_active = false;
}

bool FoliagePass::load_scatter_set(const std::filesystem::path& json_path) {
    std::ifstream in(json_path);
    if (!in) {
        LUMINUMBRA_CORE_WARN("FoliagePass: scatter set not found '{}'.", json_path.string());
        return false;
    }
    m_archetypes.clear();
    try {
        nlohmann::json doc = nlohmann::json::parse(in);
        if (!doc.contains("archetypes") || !doc["archetypes"].is_array()) {
            LUMINUMBRA_CORE_WARN("FoliagePass: scatter set '{}' missing 'archetypes' array.",
                                 json_path.string());
            return false;
        }
        for (const auto& node : doc["archetypes"]) {
            ArchetypeData a;
            a.name = node.value("name", std::string("archetype"));
            if (node.contains("color") && node["color"].is_array() && node["color"].size() == 3) {
                a.color = glm::vec3(node["color"][0].get<float>(),
                                    node["color"][1].get<float>(),
                                    node["color"][2].get<float>());
            }
            a.half_width = node.value("half_width", a.half_width);
            a.height = node.value("height", a.height);
            a.sways = node.value("sways", a.sways);
            a.density_weight = std::max(0.0f, node.value("density_weight", 1.0f));
            a.loaded = true;
            m_archetypes.push_back(std::move(a));
        }
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN(
            "FoliagePass: failed to parse scatter set '{}': {}", json_path.string(), e.what());
        m_archetypes.clear();
        return false;
    }
    if (m_archetypes.empty()) {
        return false;
    }
    m_enabled = true;
    ++m_chunk_cache_gen; // archetypes changed -> invalidate the per-chunk instance cache
    LUMINUMBRA_CORE_INFO("FoliagePass: loaded {} scatter archetypes from '{}'.",
                         m_archetypes.size(),
                         json_path.string());
    return true;
}

void FoliagePass::rebuild_instances(const std::vector<ChunkScatter>& chunks,
                                    SurfaceQuery query,
                                    void* query_ctx,
                                    const glm::vec3& camera_pos) {
    // drain any COMPLETED async blade readback into
    // m_instances EVERY frame, BEFORE the scatter-cache elision below. The GPU
    // blade buffer is unchanged while a build is elided, so a readback submitted on
    // the last non-elided frame stays valid; draining it here (not only inside
    // rebuild_instances_gpu, which the elision skips) keeps instance_hash/coverage
    // populated on static frames. Stale-safe: m_instances is replaced only when a
    // newer result arrives, so it is never re-emptied once primed.
    if (m_readback_enabled) {
        poll_foliage_readback();
    }

    // Scatter cache: the instance set is a pure function of the visible chunk-set,
    // the camera chunk (the per-chunk fade cull), and the wind. It is independent of the
    // frame otherwise (the sway WAVING is animated shader-side by u_time; aSway is just
    // the wind vector). So fold those inputs into a signature and skip the rebuild when
    // unchanged — most frames the camera has not crossed a ~16 m cell and no chunk
    // streamed, so this elides the per-frame CPU rebuild that capped density. On a skip
    // the ring buffer + m_frame_instance_count from the last build are reused as-is.
    {
        auto cell = [](float v) {
            return static_cast<long long>(std::floor(v / 16.0f));
        };
        std::uint64_t sig = 1469598103934665603ull;
        auto mix = [&sig](std::uint64_t v) {
            sig ^= v;
            sig *= 1099511628211ull;
        };
        mix(m_enabled ? 0x9E3779B97F4A7C15ull : 0x1ull);
        mix(static_cast<std::uint64_t>(cell(camera_pos.x)) * 73856093ull ^
            (static_cast<std::uint64_t>(cell(camera_pos.z)) * 19349663ull));
        // wind is in the rebuild signature ONLY in gate mode
        // (m_readback_enabled). The wind field drifts every frame, so including
        // it in normal play forced a full scatter re-bake (~5-9 ms: surface
        // re-queries + SSBO re-uploads) just to refresh the baked per-instance
        // sway vector — and the visible waving is u_time-driven shader-side, so
        // a one-rebuild-stale baked sway direction is visually negligible.
        // Dropping it lets the scatter cache ELIDE on a static/slow camera.
        // The FoliageInstancing gate, however, deliberately rebuilds with calm
        // vs windy wind and compares the baked sway (its wind-bridge isolation),
        // so in gate mode (readback on) wind MUST stay in the sig or the windy
        // rebuild would elide and the sway test would see no delta..
        if (m_readback_enabled) {
            mix(static_cast<std::uint64_t>(std::llround(m_wind_xz.x * 2.0f)) ^
                (static_cast<std::uint64_t>(std::llround(m_wind_xz.y * 2.0f)) << 16));
        }
        mix(static_cast<std::uint64_t>(std::llround(m_fade_start_m)) ^
            (static_cast<std::uint64_t>(std::llround(m_fade_end_m)) << 20));
        mix(static_cast<std::uint64_t>(std::llround(m_density_scale * 100.0f)));
        std::uint64_t chunk_acc = chunks.size();
        for (const ChunkScatter& c : chunks) {
            const std::uint64_t ch =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.x)) *
                 73856093ull) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.y)) *
                 19349663ull) ^
                (static_cast<std::uint64_t>(c.biome_id) << 40) ^
                (static_cast<std::uint64_t>(std::llround(c.density * 16.0f)) << 48);
            chunk_acc ^= splitmix64(ch); // XOR fold -> order-independent over the chunk list
        }
        mix(chunk_acc);
        if (m_scatter_built && sig == m_last_scatter_sig && !m_foliage_build_backlog) {
            return; // unchanged -> reuse the last build (ring VBO + frame_instance_count)
        }
        m_last_scatter_sig = sig;
        m_scatter_built = true;
    }

    if (!m_enabled || m_archetypes.empty() || query == nullptr) {
        m_instances.clear();
        m_frame_instance_count = 0;
        return;
    }

    //  #4: GPU scatter path. Generates the records on the GPU (compute + SSBO);
    // execute draws from the SSBO directly via glDrawArraysIndirect.
    // it submits the blade readback through the async ring, but does NOT
    // populate m_instances itself -- that is the per-frame poll_foliage_readback
    // drain above (stale-safe), so m_instances is deliberately NOT cleared here.
    if (m_gpu_scatter && rebuild_instances_gpu(chunks, query, query_ctx, camera_pos)) {
        return;
    }
    m_gpu_active = false;

    // CPU fallback path: builds m_instances directly this frame.
    m_instances.clear();
    m_frame_instance_count = 0;

    //  implementation note: emit from the per-chunk CAMERA-INDEPENDENT cache. Each renderable
    //  chunk's
    // records are built once (the expensive SurfaceQuery + hashing) and reused as the camera moves;
    // this loop only applies the cheap camera distance-fade culls and a fresh wind sway. The emit
    // order (chunk order, then candidate-index order) and the global kMaxInstances truncation match
    // the old single-pass loop exactly, so m_instances is byte-identical ->  / gate-safe.
    // Budget the uncached chunk BUILDS this frame (see m_foliage_build_backlog). Cached chunks are
    // free to emit; only first-time builds (the SurfaceQuery cost) are rate-limited so a
    // fast-moving streaming burst fades foliage in over a few frames instead of one ~960ms hitch.
    constexpr int kMaxChunkBuildsPerFrame = 4;
    int builds_left = kMaxChunkBuildsPerFrame;
    bool deferred_any = false;
    for (const ChunkScatter& chunk : chunks) {
        if (chunk.density <= 0.0f) {
            continue;
        }
        // Distance cull whole chunks past the fade horizon (no per-instance work
        // for far tiles -- gate: no foliage beyond the live ring).
        const glm::vec3 chunk_center =
            chunk.origin + glm::vec3(chunk.extent_m * 0.5f, 0.0f, chunk.extent_m * 0.5f);
        const float chunk_dist =
            std::sqrt((chunk_center.x - camera_pos.x) * (chunk_center.x - camera_pos.x) +
                      (chunk_center.z - camera_pos.z) * (chunk_center.z - camera_pos.z));
        if (chunk_dist - chunk.extent_m > m_fade_end_m) {
            continue;
        }

        // Use the cached records if present+current; otherwise build at most
        // kMaxChunkBuildsPerFrame new chunks this frame and defer the rest (they fade in over
        // subsequent frames).
        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.x))) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.y)) << 32);
        const std::vector<InstanceRecord>* cachedp = nullptr;
        auto cit = m_chunk_cache.find(key);
        if (cit != m_chunk_cache.end() && cit->second.gen == m_chunk_cache_gen) {
            cachedp = &cit->second.records;
        } else if (builds_left > 0) {
            cachedp = &build_or_get_chunk_records(chunk, query, query_ctx);
            --builds_left;
        } else {
            deferred_any = true; // over budget this frame -> build on a later frame
            continue;
        }

        const std::vector<InstanceRecord>& cached = *cachedp;
        for (const InstanceRecord& src : cached) {
            if (m_instances.size() >= kMaxInstances) {
                break;
            }
            // Per-instance distance-fade hard cull (camera-dependent; cheap).
            const float dxc = src.pos[0] - camera_pos.x;
            const float dzc = src.pos[2] - camera_pos.z;
            if (std::sqrt(dxc * dxc + dzc * dzc) > m_fade_end_m) {
                continue;
            }
            InstanceRecord rec = src;
            // sway rides FRESH from the current wind (cached record bakes 0). sway_scale (0/1) is
            // preserved in color[3] (to_unorm8 -> 0 or 255), so sway = wind * scale reproduces the
            // uncached loop's baked vector exactly.
            if (rec.color[3] != 0) {
                rec.sway[0] = m_wind_xz.x;
                rec.sway[1] = m_wind_xz.y;
            } else {
                rec.sway[0] = 0.0f;
                rec.sway[1] = 0.0f;
            }
            m_instances.push_back(rec);
        }
        if (m_instances.size() >= kMaxInstances) {
            break;
        }
    }

    // Prune cache entries whose chunk is no longer renderable once it grows past a soft cap, so a
    // long traverse doesn't accumulate per-chunk records without bound.
    if (m_chunk_cache.size() > 768) {
        for (auto it = m_chunk_cache.begin(); it != m_chunk_cache.end();) {
            bool live = false;
            for (const ChunkScatter& c : chunks) {
                const std::uint64_t k =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.x))) |
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.y)) << 32);
                if (k == it->first) {
                    live = true;
                    break;
                }
            }
            it = live ? std::next(it) : m_chunk_cache.erase(it);
        }
    }

    // Suppress scatter-cache elision next frame while builds remain pending, so the rebuild keeps
    // draining the backlog (fading the rest of the foliage in) even if the camera holds still.
    m_foliage_build_backlog = deferred_any;

    map_instances_for_frame();
}

//  implementation note: build (or fetch the cached) camera-independent instance records for one
//  chunk.
// This is the moved body of the old per-instance scatter loop, MINUS the camera distance cull
// (applied by the caller) and with sway baked as 0 (set fresh per frame at copy). Pure function of
// chunk_xz + the static terrain surface query + m_density_scale + m_archetypes (keyed by chunk_xz,
// invalidated by m_chunk_cache_gen).; produces byte-identical output to the old single-pass loop.
const std::vector<FoliagePass::InstanceRecord>& FoliagePass::build_or_get_chunk_records(
    const ChunkScatter& chunk, SurfaceQuery query, void* query_ctx) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.x))) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.y)) << 32);
    auto it = m_chunk_cache.find(key);
    if (it != m_chunk_cache.end() && it->second.gen == m_chunk_cache_gen) {
        return it->second.records;
    }
    CachedChunkRecords& slot = m_chunk_cache[key];
    slot.gen = m_chunk_cache_gen;
    slot.records.clear();

    // Total archetype weight for the deterministic per-instance archetype pick.
    float total_weight = 0.0f;
    for (const auto& a : m_archetypes) {
        total_weight += a.density_weight;
    }
    if (total_weight <= 0.0f) {
        total_weight = 1.0f;
    }

    {
        // Number of candidate slots scales with density (capped). Each candidate
        // is a deterministic hash draw; slope/moisture decide the emit.
        const std::size_t candidates = std::min<std::size_t>(
            kMaxCandidatesPerChunk,
            static_cast<std::size_t>(std::lround(
                chunk.density * static_cast<float>(kMaxCandidatesPerChunk) * m_density_scale)));

        for (uint32_t idx = 0; idx < candidates; ++idx) {
            const uint64_t h0 =
                placement_hash(chunk.chunk_xz.x, chunk.chunk_xz.y, chunk.biome_id, idx);
            const uint64_t h1 = splitmix64(h0 ^ 0x2545F4914F6CDD1Dull);
            const uint64_t h2 = splitmix64(h1 ^ 0x9E3779B97F4A7C15ull);
            const uint64_t h3 = splitmix64(h2 ^ 0xBF58476D1CE4E5B9ull);

            // Jittered position inside the chunk footprint.
            const float fx = hash_unit(h0);
            const float fz = hash_unit(h1);
            const float wx = chunk.origin.x + fx * chunk.extent_m;
            const float wz = chunk.origin.z + fz * chunk.extent_m;

            // NOTE: the per-instance distance-fade hard cull lives in the CALLER now (it depends on
            // the camera); these cached records are camera-independent. The caller skips any record
            // whose anchor is past m_fade_end_m before emitting, preserving the ring-bounded set.

            const SurfaceSample surf = query(query_ctx, wx, wz);
            if (!surf.valid) {
                continue; // underwater / no ground here
            }

            // HARD placement gates so the
            // scatter only ever lands on WALKABLE LAND. These are belt-and-braces
            // on top of the surface query (which already rejects underwater
            // columns): a card must never float on the water surface nor cling to a
            // steep cliff face.
            //   * WATER gate: skip any anchor at or below sea level. (The query
            //     marks underwater columns invalid, but a shoreline sample can sit
            //     a hair above the query's threshold yet still read as "on water";
            //     the explicit sea-level reject removes the floaters seen plastered
            //     on the water in the sweep.)
            //   * SLOPE gate: skip steep ground above kMaxFoliageSlope. The slope is
            //     a 0..1 rise-over-run estimate; cliffs/dune faces shed all foliage
            //     so cards stop appearing pasted on the conical hillsides.
            constexpr float kSeaLevel = 0.0f;         // Luminumbra::SEA_LEVEL
            constexpr float kWaterMargin = 0.4f;      // keep blades off the wet fringe
            constexpr float kMaxFoliageSlope = 0.70f; // ~35deg; steeper = bare cliff
            if (surf.height <= kSeaLevel + kWaterMargin) {
                continue; // on/at water -> no ground cover
            }
            if (surf.slope >= kMaxFoliageSlope) {
                continue; // too steep -> bare dirt/cliff
            }

            // DENSITY MODULATION (documented design): biome density modulated
            // by slope (steep ground sheds foliage) and moisture (wet ground
            // grows more). The per-candidate accept threshold is a hash draw, so
            // the placement stays a pure function of the world grid.
            // the slope falloff is now
            // sharpened (square of the remaining headroom under the cutoff) so
            // gentle ground stays FULLY covered (no bald patches) while ground
            // approaching the cutoff thins out smoothly instead of abruptly. The
            // moisture term keeps a high floor so suitable flat land reads as
            // CONTINUOUS cover rather than scattered tufts.
            const float slope_head = std::clamp(1.0f - surf.slope / kMaxFoliageSlope, 0.0f, 1.0f);
            const float slope_factor = slope_head * slope_head;
            const float moisture_factor = std::clamp(0.78f + 0.22f * surf.moisture, 0.0f, 1.0f);
            // Boost the effective density so flat, suitable ground reaches near-full
            // candidate acceptance (continuous cover), still clamped to [0,1] so the
            // per-chunk candidate budget remains the hard ceiling..
            // raised the multiplier so the
            // near-field ground reads as CONTINUOUS cover (no bald patches) in the
            // down-pitched cells across all times of day.
            const float accept =
                std::clamp(chunk.density * 2.4f * m_density_scale * slope_factor * moisture_factor,
                           0.0f,
                           1.0f);
            if (hash_unit(h2) > accept) {
                continue;
            }

            // Deterministic per-instance archetype pick (weighted).
            float pick = hash_unit(h3) * total_weight;
            std::size_t arch_index = 0;
            for (std::size_t a = 0; a < m_archetypes.size(); ++a) {
                if (pick < m_archetypes[a].density_weight) {
                    arch_index = a;
                    break;
                }
                pick -= m_archetypes[a].density_weight;
                arch_index = a;
            }
            const ArchetypeData& arch = m_archetypes[arch_index];

            // Per-archetype sway FLAG (0/1). Pebbles/clutter (sways=false) never  The actual
            // sway VECTOR is set by the caller per frame from the live wind (so a re-cached chunk
            // never carries a stale wind direction); here it bakes 0 and rides the flag in alpha.
            const float sway_scale = arch.sways ? 1.0f : 0.0f;

            InstanceRecord rec;
            rec.pos[0] = wx;
            rec.pos[1] = surf.height;
            rec.pos[2] = wz;
            // Slight per-instance size jitter (deterministic).
            const float size_jit = 0.8f + 0.4f * hash_unit(splitmix64(h3 ^ 0x123456789ABCDEFull));
            rec.size[0] = arch.half_width * size_jit;
            rec.size[1] = arch.height * size_jit;

            // per-instance TONAL variation so
            // the field is not a flat single neon hue. A deterministic value jitter
            // (darker/lighter) plus a small green<->khaki hue jitter breaks up the
            // billboard banding; the vertex/frag stage further darkens the ROOT of
            // each card so blades read with a base-to-tip gradient. The base albedo
            // is already desaturated in the JSON; here we only spread it. The result
            // stays well clear of the speckle detector's green-dominance margin.
            const float val_jit = 0.72f + 0.42f * hash_unit(splitmix64(h2 ^ 0xC2B2AE3D27D4EB4Full));
            const float hue_jit =
                (hash_unit(splitmix64(h1 ^ 0x165667B19E3779F9ull)) - 0.5f) * 0.10f;
            float cr = arch.color.r * val_jit + hue_jit; // toward khaki when +
            float cg = arch.color.g * val_jit;
            float cb = arch.color.b * val_jit - 0.4f * hue_jit; // away from blue when +
            rec.color[0] = to_unorm8(cr);
            rec.color[1] = to_unorm8(cg);
            rec.color[2] = to_unorm8(cb);
            rec.color[3] = to_unorm8(sway_scale); // sway-flag scale rides in alpha
            rec.sway[0] = 0.0f; // set fresh per frame by the caller from the live wind
            rec.sway[1] = 0.0f;
            rec.phase = encode_f16(hash_unit(h1) * 6.2831853f);
            rec.facing = encode_f16(hash_unit(h0) * 6.2831853f);
            slot.records.push_back(rec);
        }
    }

    return slot.records;
}

bool FoliagePass::rebuild_instances_gpu(const std::vector<ChunkScatter>& chunks,
                                        SurfaceQuery query,
                                        void* query_ctx,
                                        const glm::vec3& camera_pos) {
    if (m_compute_prog == 0 || m_blade_ssbo == 0) {
        return false;
    }

    // --- Build the per-chunk param + surface-grid uploads on the CPU. Only
    // chunks with density > 0 within the fade ring are uploaded (matches the CPU
    // whole-chunk cull). The surface is sampled on a coarse (kSurfaceGridVerts^2)
    // grid -- ~81 queries/chunk instead of up to kMaxCandidatesPerChunk. ---
    struct GpuChunk {
        float origin_extent[4];
        float id_density[4];
    };
    std::vector<GpuChunk> chunk_params;
    std::vector<glm::vec4> surf_grid; // (height, moisture, slope, valid) per grid vert
    chunk_params.reserve(chunks.size());
    const int gv = kSurfaceGridVerts;

    //  implementation note: the per-chunk surface grid is the expensive part (kSurfaceGridVerts^2
    // GetTerrainHeightAt calls) and is CAMERA-INDEPENDENT (pure function of chunk_xz + static
    // terrain). Cache it per chunk and only BUILD a budgeted few new chunks per frame so a fast
    // streaming burst doesn't re-sample every chunk's grid at once (~1s hitch). Deferred chunks
    // contribute no foliage this frame and build over the next few (foliage fades in — ).
    // In gate mode (m_readback_enabled) build everything (no defer) so the FoliageInstancing gate's
    // instance_hash sees the full, exact scatter.
    const int gv2 = gv * gv;
    const int kMaxSurfBuildsPerFrame = m_readback_enabled ? std::numeric_limits<int>::max() : 4;
    int builds_left = kMaxSurfBuildsPerFrame;
    bool deferred_any = false;
    for (const ChunkScatter& chunk : chunks) {
        if (chunk.density <= 0.0f) {
            continue;
        }
        const glm::vec3 cc =
            chunk.origin + glm::vec3(chunk.extent_m * 0.5f, 0.0f, chunk.extent_m * 0.5f);
        const float cd = std::sqrt((cc.x - camera_pos.x) * (cc.x - camera_pos.x) +
                                   (cc.z - camera_pos.z) * (cc.z - camera_pos.z));
        if (cd - chunk.extent_m > m_fade_end_m) {
            continue;
        }

        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.x))) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(chunk.chunk_xz.y)) << 32);
        const std::vector<glm::vec4>* gridp = nullptr;
        auto git = m_surf_grid_cache.find(key);
        if (git != m_surf_grid_cache.end()) {
            gridp = &git->second;
        } else if (builds_left > 0) {
            // Build (and cache) this chunk's surface grid. Grid vert (gx,gy) maps to local [0,1]^2.
            std::vector<glm::vec4> grid;
            grid.reserve(static_cast<std::size_t>(gv2));
            for (int gy = 0; gy < gv; ++gy) {
                for (int gx = 0; gx < gv; ++gx) {
                    const float lx = static_cast<float>(gx) / static_cast<float>(kSurfaceGrid);
                    const float lz = static_cast<float>(gy) / static_cast<float>(kSurfaceGrid);
                    const float wx = chunk.origin.x + lx * chunk.extent_m;
                    const float wz = chunk.origin.z + lz * chunk.extent_m;
                    const SurfaceSample s = query(query_ctx, wx, wz);
                    grid.emplace_back(s.height, s.moisture, s.slope, s.valid ? 1.0f : 0.0f);
                }
            }
            gridp = &m_surf_grid_cache.emplace(key, std::move(grid)).first->second;
            --builds_left;
        } else {
            deferred_any = true; // over budget this frame -> build on a later frame
            continue;
        }

        GpuChunk gc;
        gc.origin_extent[0] = chunk.origin.x;
        gc.origin_extent[1] = chunk.origin.y;
        gc.origin_extent[2] = chunk.origin.z;
        gc.origin_extent[3] = chunk.extent_m;
        gc.id_density[0] = static_cast<float>(chunk.chunk_xz.x);
        gc.id_density[1] = static_cast<float>(chunk.chunk_xz.y);
        gc.id_density[2] = static_cast<float>(chunk.biome_id);
        gc.id_density[3] = chunk.density;
        chunk_params.push_back(gc);
        surf_grid.insert(surf_grid.end(), gridp->begin(), gridp->end());
    }

    // Suppress scatter-cache elision next frame while builds remain pending (drain the backlog even
    // if the camera holds still); prune cache entries for chunks no longer renderable past a soft
    // cap.
    m_foliage_build_backlog = deferred_any;
    if (m_surf_grid_cache.size() > 768) {
        for (auto it = m_surf_grid_cache.begin(); it != m_surf_grid_cache.end();) {
            bool live = false;
            for (const ChunkScatter& c : chunks) {
                const std::uint64_t k =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.x))) |
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.chunk_xz.y)) << 32);
                if (k == it->first) {
                    live = true;
                    break;
                }
            }
            it = live ? std::next(it) : m_surf_grid_cache.erase(it);
        }
    }

    const int chunk_count = static_cast<int>(chunk_params.size());
    if (chunk_count == 0) {
        // Nothing to scatter (all chunks culled). Empty build; gate sees 0.
        const GLuint empty_draw[5] = {0u, 12u, 0u, 0u, 0u};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_count_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(empty_draw), empty_draw);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        m_instances.clear();
        m_frame_instance_count = 0;
        m_gpu_active = true;
        return true;
    }

    // Archetype palette: 2 vec4 per archetype.
    float total_weight = 0.0f;
    for (const auto& a : m_archetypes)
        total_weight += a.density_weight;
    if (total_weight <= 0.0f)
        total_weight = 1.0f;
    std::vector<float> pal;
    pal.reserve(m_archetypes.size() * 8);
    for (const auto& a : m_archetypes) {
        pal.push_back(a.color.r);
        pal.push_back(a.color.g);
        pal.push_back(a.color.b);
        pal.push_back(a.density_weight);
        pal.push_back(a.half_width);
        pal.push_back(a.height);
        pal.push_back(a.sways ? 1.0f : 0.0f);
        pal.push_back(0.0f);
    }

    // --- Upload. ---
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_chunk_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(chunk_params.size() * sizeof(GpuChunk)),
                 chunk_params.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_surf_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(surf_grid.size() * sizeof(glm::vec4)),
                 surf_grid.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_arch_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(pal.size() * sizeof(float)),
                 pal.data(),
                 GL_DYNAMIC_DRAW);
    const GLuint draw_command[5] = {0u, 12u, 0u, 0u, 0u};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_count_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(draw_command), draw_command);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // --- Dispatch. ---
    glUseProgram(m_compute_prog);
    glUniform1i(glGetUniformLocation(m_compute_prog, "u_chunk_count"), chunk_count);
    glUniform1i(glGetUniformLocation(m_compute_prog, "u_archetype_count"),
                static_cast<int>(m_archetypes.size()));
    glUniform1f(glGetUniformLocation(m_compute_prog, "u_total_weight"), total_weight);
    glUniform1i(glGetUniformLocation(m_compute_prog, "u_max_candidates"),
                static_cast<int>(kMaxCandidatesPerChunk));
    glUniform1ui(glGetUniformLocation(m_compute_prog, "u_max_instances"),
                 static_cast<GLuint>(kMaxInstances));
    glUniform1f(glGetUniformLocation(m_compute_prog, "u_density_scale"), m_density_scale);
    glUniform2f(glGetUniformLocation(m_compute_prog, "u_camera_xz"), camera_pos.x, camera_pos.z);
    glUniform1f(glGetUniformLocation(m_compute_prog, "u_fade_end_m"), m_fade_end_m);
    glUniform2f(glGetUniformLocation(m_compute_prog, "u_wind_xz"), m_wind_xz.x, m_wind_xz.y);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_chunk_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_surf_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_blade_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_count_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_arch_ssbo);

    const GLuint groups_x = static_cast<GLuint>((kMaxCandidatesPerChunk + 63) / 64);
    glDispatchCompute(groups_x, static_cast<GLuint>(chunk_count), 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT |
                    GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    glUseProgram(0);

    // --- Read the count + the generated blades back into m_instances. This is
    // ONLY needed by the FoliageInstancing gate's instance_hash — execute
    // draws straight from m_blade_ssbo via glDrawArraysIndirect (the count lives
    // in m_count_ssbo, GPU-resident).  : this readback used to
    // be a synchronous glGetBufferSubData that blocked the CPU on compute
    // completion (~5 ms on the dense pose). It now routes through the async
    // AsyncReadbackRing — submit issues the GPU->CPU copy + a fence and returns
    // immediately; consume yields the most-recent COMPLETED result a later frame
    // (stale-safe), never stalling. The indirect draw uses the true GPU count
    // regardless, so play/benchmark still disable the readback entirely. ---
    if (m_readback_enabled) {
        // SUBMIT this frame's blade readback into the async ring: copy
        // the count (draw_instance_count at byte offset sizeof(GLuint)*2 in
        // m_count_ssbo) + the blade buffer GPU->slot and fence. Returns immediately
        // (no glGetBufferSubData stall). The COMPLETED result is drained into
        // m_instances by poll_foliage_readback (called every frame from
        // rebuild_instances), so it survives the scatter-cache elision.
        constexpr std::size_t kSlotBytes =
            kReadbackCountBytes + kMaxInstances * sizeof(InstanceRecord);
        if (m_readback_ring.ensure(kSlotBytes, 3) && m_readback_ring.begin()) {
            m_readback_ring.copy_region(m_count_ssbo, sizeof(GLuint) * 2, 0, kReadbackCountBytes);
            m_readback_ring.copy_region(m_blade_ssbo,
                                        0,
                                        static_cast<std::ptrdiff_t>(kReadbackCountBytes),
                                        kMaxInstances * sizeof(InstanceRecord));
            m_readback_ring.submit();
        }
    } else {
        // Play / benchmark: no readback. m_instances is gate-only and unused in this
        // mode, so keep it empty (mirrors the prior synchronous path's else branch).
        m_instances.clear();
    }
    // m_frame_instance_count drives execute's draw guard + the foliage-instances
    // stat. In readback mode it tracks the CPU-known (stale) count from the ring
    // drain (m_instances), so the stat stays honest -- 0 until the first readback
    // primes it, exactly like the prior synchronous path. In play mode the CPU
    // count is unused (the indirect draw uses the GPU-resident count), so a
    // non-zero marker just lets execute proceed.
    m_frame_instance_count =
        m_readback_enabled ? m_instances.size() : static_cast<std::size_t>(kMaxInstances);
    m_gpu_active = true;
    return true;
}

void FoliagePass::poll_foliage_readback() {
    // drain the most-recent COMPLETED blade readback into
    // m_instances (stale-safe). Replaces m_instances only when a NEWER result
    // arrives, so it is never re-emptied once primed -- keeping instance_hash and
    // the FoliageInstancing coverage probe non-empty even across cache-elided
    // frames..
    const void* slot = nullptr;
    std::size_t slot_bytes = 0;
    if (!m_readback_ring.consume(&slot, &slot_bytes) || slot == nullptr) {
        return; // no newer completed result this frame -> keep the held set
    }
    GLuint count = 0;
    std::memcpy(&count, slot, kReadbackCountBytes);
    count = std::min<GLuint>(count, static_cast<GLuint>(kMaxInstances));
    const InstanceRecord* blades = reinterpret_cast<const InstanceRecord*>(
        static_cast<const char*>(slot) + kReadbackCountBytes);
    m_instances.assign(blades, blades + count);
    // Keep the draw guard / foliage-instances stat synced with the drained set even
    // on scatter-cache-elided frames (where rebuild_instances_gpu does not run).
    m_frame_instance_count = m_instances.size();
}

void FoliagePass::map_instances_for_frame() {
    m_ring_cursor = (m_ring_cursor + 1) % kRingFrames;
    InstanceRecord* dst = m_instance_ptr[m_ring_cursor];
    if (dst == nullptr) {
        m_frame_instance_count = 0;
        return;
    }
    const std::size_t count = std::min(m_instances.size(), kMaxInstances);
    if (count > 0) {
        std::memcpy(dst, m_instances.data(), count * sizeof(InstanceRecord));
    }
    m_frame_instance_count = count;
}

std::size_t FoliagePass::execute(const RenderContext& ctx, const Camera& camera) {
    if (!m_enabled || !m_shader || !m_shader->IsValid() || m_frame_instance_count == 0 ||
        m_vao == 0) {
        return 0;
    }

    if (!ctx.lit_scene.id) {
        return 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, ctx.lit_scene.id);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // forward foliage into the internal scene

    // Opaque-ish ground cover: depth test AND write against the scene depth so
    // the cards occlude correctly, alpha-tested in the frag shader. Blend on for
    // soft edges.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
    if (cull_was_enabled) {
        glDisable(GL_CULL_FACE); // two-sided cards
    }

    m_shader->use();
    const glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                                  static_cast<float>(ctx.screen_width) /
                                                      static_cast<float>(ctx.screen_height),
                                                  camera.GetNearPlane(),
                                                  camera.GetFarPlane());
    const glm::mat4 view = camera.GetViewMatrix();
    m_shader->setMat4("u_view", view);
    m_shader->setMat4("u_projection", projection);
    m_shader->setVec3("u_cameraPos", camera.Position);
    m_shader->setFloat("u_time", ctx.time_seconds);
    m_shader->setFloat("u_swayAmplitude", m_sway_amplitude);
    m_shader->setFloat("u_swaySpeed", m_sway_speed);
    m_shader->setFloat("u_fadeStart", m_fade_start_m);
    m_shader->setFloat("u_fadeEnd", m_fade_end_m);

    m_shader->setVec3("u_sunDirection", ctx.sun.direction);
    m_shader->setVec3("u_sunColor", ctx.sun.color);
    m_shader->setFloat("u_sunIntensity", ctx.sun.intensity);
    m_shader->setVec3("u_ambientColor", ctx.sky_ambient_color);
    // foliage-night: feed the SAME moon toward-light direction the deferred lighting
    // pass keys its moon term off (moon-shadows workstream), so night blades pick up
    // the identical cool moonlit tone as the terrain instead of glowing.
    m_shader->setVec3("u_moonDir", ctx.moon_light_dir);

    //  feed the SAME projected cloud cast-shadow state the
    // lighting pass uses, so storm-overcast cells drive the blades DARK like the
    // terrain (no more teal glow under storm). Cleanly disabled when clouds are
    // off (enabled==0 -> the shader's cloud term is a no-op).
    const Luminumbra::Rendering::CloudRenderState& cloud = ctx.cloud_state;
    const bool cloud_on = cloud.enabled && cloud.shadow_enabled;
    m_shader->setInt("u_cloudShadowEnabled", cloud_on ? 1 : 0);
    m_shader->setVec2("u_cloudScrollOffset", cloud.scroll_offset);
    m_shader->setFloat("u_cloudCoverageAmount", cloud.coverage_amount);
    m_shader->setFloat("u_cloudBiomeVariation", cloud.biome_variation);
    m_shader->setFloat("u_cloudPlaneHeight", cloud.plane_height);
    m_shader->setFloat("u_cloudShadowStrength", cloud.shadow_strength);
    m_shader->setVec3("u_cloudSunDir", cloud.sun_travel_dir);

    glBindVertexArray(m_vao);
    //  #4/: when the GPU scatter path built this frame, draw straight
    // from the blade SSBO using the compute-written indirect command. The CPU
    // fallback path still uses the persistent-mapped ring buffer.
    if (m_gpu_active && m_count_ssbo != 0) {
        glBindVertexBuffer(0, m_blade_ssbo, 0, sizeof(InstanceRecord));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_count_ssbo);
        glDrawArraysIndirect(GL_TRIANGLES,
                             reinterpret_cast<const void*>(kGrassDrawCommandOffsetBytes));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    } else {
        glBindVertexBuffer(0, m_instance_vbo[m_ring_cursor], 0, sizeof(InstanceRecord));
        //  12 verts/instance = two crossed quads (6 verts
        // each) so a blade reads as upright cover from any angle, not a flat decal.
        glDrawArraysInstanced(GL_TRIANGLES, 0, 12, static_cast<GLsizei>(m_frame_instance_count));
    }
    glBindVertexArray(0);

    if (cull_was_enabled) {
        glEnable(GL_CULL_FACE);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    return m_frame_instance_count;
}

uint64_t FoliagePass::instance_hash() const {
    if (m_instances.empty()) {
        return fnv1a(nullptr, 0);
    }
    return fnv1a(m_instances.data(), m_instances.size() * sizeof(InstanceRecord));
}

std::size_t FoliagePass::instances_within(const glm::vec3& center, float radius_m) const {
    const float r2 = radius_m * radius_m;
    std::size_t count = 0;
    for (const auto& rec : m_instances) {
        const float dx = rec.pos[0] - center.x;
        const float dz = rec.pos[2] - center.z;
        if (dx * dx + dz * dz <= r2) {
            ++count;
        }
    }
    return count;
}

std::size_t FoliagePass::instances_beyond(const glm::vec3& center, float radius_m) const {
    const float r2 = radius_m * radius_m;
    std::size_t count = 0;
    for (const auto& rec : m_instances) {
        const float dx = rec.pos[0] - center.x;
        const float dz = rec.pos[2] - center.z;
        if (dx * dx + dz * dz > r2) {
            ++count;
        }
    }
    return count;
}

float FoliagePass::max_sway_displacement() const {
    float max_mag = 0.0f;
    for (const auto& rec : m_instances) {
        const float mag = std::sqrt(rec.sway[0] * rec.sway[0] + rec.sway[1] * rec.sway[1]);
        max_mag = std::max(max_mag, mag);
    }
    return max_mag;
}

} // namespace Luminumbra::Rendering
