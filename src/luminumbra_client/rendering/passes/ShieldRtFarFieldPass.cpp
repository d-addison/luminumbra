#include "rendering/passes/ShieldRtFarFieldPass.h"

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Luminumbra::Rendering {

namespace {

// Fullscreen triangle (no VBO).
const char* kFullscreenVert = R"GLSL(
#version 450 core
void main() {
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Far-field G-buffer raymarch (validated as ShieldRtFarFieldGbufferGpu /
// ShieldRtFarFieldParityGpu). Reconstructs a per-pixel world ray from the inverse
// view-projection, marches the heightfield with the overshoot-free hierarchical
// DDA, and on a hit writes the deferred G-buffer + gl_FragDepth; miss -> discard.
const char* kRaymarchFrag = R"GLSL(
#version 450 core

layout(location = 0) out vec3 gPosition;        // view-space position
layout(location = 1) out vec4 gNormalMaterial;  // oct view-space normal + matId/255
layout(location = 2) out vec4 gAlbedoRoughness;
layout(location = 3) out vec2 gMetallicAO;

layout(std430, binding = 1) readonly buffer HfBuf { float hf[]; };
layout(std430, binding = 2) readonly buffer MipBuf{ float mip[]; };

uniform vec2  u_viewport;
uniform vec3  u_eye;
uniform mat4  u_invViewProj;
uniform mat4  u_view;
uniform mat4  u_viewProj;
uniform mat3  u_normalView;
uniform int   u_n;
uniform float u_step;
uniform vec2  u_origin;    // ox, oz
uniform float u_tmax;
uniform int   u_levels;
uniform int   u_maxSteps;
uniform float u_seaLevel;
uniform int   u_mipOffset[24];
uniform int   u_mipDim[24];
// inc2c-SCALE step 1: far-pixel-only dispatch. A copy of the scene depth (blitted
// before the pass to avoid a feedback loop on the depth attachment this pass writes
// via gl_FragDepth). < 1.0 means opaque geometry (live/far-LOD mesh) already covered
// this pixel; the GL_LESS depth test would reject the raymarch write there anyway, so
// we discard BEFORE marching and skip the wasted rays. Typical down/eye views are
// ~2/3 near-terrain -> most rays skipped. Quality-neutral (augment-not-replace,
// Decision A: the raymarch fills only the sky/gap pixels the mesh did not).
uniform sampler2D u_sceneDepth;
uniform int   u_farPixelEarlyOut;  // 1 = enabled (0 keeps the validated full-frame march)

float hfSample(float wx, float wz) {
    float fx = clamp((wx - u_origin.x) / u_step, 0.0, float(u_n - 1));
    float fz = clamp((wz - u_origin.y) / u_step, 0.0, float(u_n - 1));
    int x0 = int(floor(fx)); int z0 = int(floor(fz));
    int x1 = min(x0 + 1, u_n - 1); int z1 = min(z0 + 1, u_n - 1);
    float tx = fx - float(x0); float tz = fz - float(z0);
    float h00 = hf[z0 * u_n + x0]; float h10 = hf[z0 * u_n + x1];
    float h01 = hf[z1 * u_n + x0]; float h11 = hf[z1 * u_n + x1];
    float a = h00 + (h10 - h00) * tx;
    float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}
float cellMaxAt(int L, float px, float pz) {
    float cs = u_step * float(1 << L);
    int dim = u_mipDim[L];
    int cx = clamp(int(floor((px - u_origin.x) / cs)), 0, dim - 1);
    int cz = clamp(int(floor((pz - u_origin.y) / cs)), 0, dim - 1);
    return mip[u_mipOffset[L] + cz * dim + cx];
}
float cellExitDist(int L, float px, float pz, float dx, float dz) {
    float cs = u_step * float(1 << L);
    float lx = (px - u_origin.x) / cs; float lz = (pz - u_origin.y) / cs;
    int cx = int(floor(lx)); int cz = int(floor(lz));
    float tx = 1.0e30; float tz = 1.0e30;
    if (abs(dx) > 1.0e-9) {
        float bound = (dx > 0.0 ? float(cx + 1) : float(cx)) * cs + u_origin.x;
        tx = (bound - px) / dx;
    }
    if (abs(dz) > 1.0e-9) {
        float bound = (dz > 0.0 ? float(cz + 1) : float(cz)) * cs + u_origin.y;
        tz = (bound - pz) / dz;
    }
    return max(min(tx, tz), 0.0);
}
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0); }
vec2 encode_octahedral(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

void main() {
    // Far-pixel early-out: skip the march where opaque geometry already won this
    // pixel (sampled scene-depth copy < far plane). Pure perf — the depth test
    // would reject the write here regardless.
    if (u_farPixelEarlyOut == 1 &&
        texelFetch(u_sceneDepth, ivec2(gl_FragCoord.xy), 0).r < 1.0) {
        discard;
    }
    vec2 uv = gl_FragCoord.xy / u_viewport;
    vec4 farp = u_invViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    farp /= farp.w;
    vec3 d = normalize(farp.xyz - u_eye);
    vec3 o = u_eye;

    float base_cell = u_step;
    float ext = float(u_n - 1) * u_step;
    float eps = base_cell * 0.01;
    float t = 0.0;
    int L = u_levels - 1;
    int steps = 0;
    float hit_t = -1.0;
    while (t < u_tmax && steps < u_maxSteps) {
        steps++;
        float px = o.x + d.x * t;
        float py = o.y + d.y * t;
        float pz = o.z + d.z * t;
        if (px < u_origin.x - base_cell || pz < u_origin.y - base_cell ||
            px > u_origin.x + ext + base_cell || pz > u_origin.y + ext + base_cell) break;
        float exit_d = cellExitDist(L, px, pz, d.x, d.z);
        float t_exit = t + exit_d;
        float py_exit = o.y + d.y * t_exit;
        float cmax = cellMaxAt(L, px, pz);
        if (py > cmax && py_exit > cmax) {
            t = t_exit + eps;
            if (L < u_levels - 1) ++L;
            continue;
        }
        if (L > 0) { --L; continue; }
        float surf_t = hfSample(px, pz);
        if (py - surf_t <= 0.0) { hit_t = t; break; }
        float px_e = o.x + d.x * t_exit;
        float pz_e = o.z + d.z * t_exit;
        if (py_exit - hfSample(px_e, pz_e) <= 0.0) {
            float lo = t; float hi = t_exit;
            for (int it = 0; it < 20; ++it) {
                float tm = 0.5 * (lo + hi);
                float mx = o.x + d.x * tm;
                float my = o.y + d.y * tm;
                float mz = o.z + d.z * tm;
                if (my - hfSample(mx, mz) <= 0.0) hi = tm; else lo = tm;
            }
            hit_t = hi;
            break;
        }
        t = t_exit + eps;
        if (L < u_levels - 1) ++L;
    }

    if (hit_t < 0.0) discard;

    vec3 P = o + d * hit_t;
    vec4 clip = u_viewProj * vec4(P, 1.0);
    gl_FragDepth = (clip.z / clip.w) * 0.5 + 0.5;
    gPosition = (u_view * vec4(P, 1.0)).xyz;

    float e = u_step;
    float hl = hfSample(P.x - e, P.z);
    float hr = hfSample(P.x + e, P.z);
    float hd = hfSample(P.x, P.z - e);
    float hu = hfSample(P.x, P.z + e);
    vec3 worldN = normalize(vec3(hl - hr, 2.0 * e, hd - hu));
    vec3 viewN = normalize(u_normalView * worldN);
    vec2 enc = encode_octahedral(viewN);
    float matId = (P.y < u_seaLevel + 1.0) ? 4.0 : (worldN.y > 0.9 ? 3.0 : 1.0);
    gNormalMaterial = vec4(enc * 0.5 + 0.5, 0.0, matId / 255.0);
    gAlbedoRoughness = vec4(0.4, 0.45, 0.3, 0.9);
    gMetallicAO = vec2(0.0, 1.0);
}
)GLSL";

// Max-mip level 0: each cell = max of its 2x2 base samples, written into the flat
// max-mip buffer at u_out_offset.
const char* kMaxMipL0Compute = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) readonly  buffer Base { float base[]; };
layout(std430, binding = 1) writeonly buffer Mip  { float mip[]; };
uniform int u_n;
uniform int u_cells;
uniform int u_out_offset;
void main() {
    int cx = int(gl_GlobalInvocationID.x);
    int cz = int(gl_GlobalInvocationID.y);
    if (cx >= u_cells || cz >= u_cells) return;
    float a = base[cz * u_n + cx];
    float b = base[cz * u_n + cx + 1];
    float c = base[(cz + 1) * u_n + cx];
    float dd = base[(cz + 1) * u_n + cx + 1];
    mip[u_out_offset + cz * u_cells + cx] = max(max(a, b), max(c, dd));
}
)GLSL";

// Max-mip coarsen: out[(fine+1)/2]^2 = max over the 2x2 fine cells, in-place in the
// flat buffer (read u_in_offset, write u_out_offset; a barrier separates levels).
const char* kMaxMipReduceCompute = R"GLSL(
#version 450 core
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, binding = 0) buffer Mip { float mip[]; };
uniform int u_fine;
uniform int u_coarse;
uniform int u_in_offset;
uniform int u_out_offset;
void main() {
    int X = int(gl_GlobalInvocationID.x);
    int Z = int(gl_GlobalInvocationID.y);
    if (X >= u_coarse || Z >= u_coarse) return;
    float m = -3.0e38;
    for (int dz = 0; dz < 2; ++dz) {
        int fz = 2 * Z + dz;
        if (fz >= u_fine) continue;
        for (int dx = 0; dx < 2; ++dx) {
            int fx = 2 * X + dx;
            if (fx >= u_fine) continue;
            m = max(m, mip[u_in_offset + fz * u_fine + fx]);
        }
    }
    mip[u_out_offset + Z * u_coarse + X] = m;
}
)GLSL";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LUMINUMBRA_CORE_ERROR("ShieldRtFarFieldPass shader compile failed: {}", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint LinkProgram(std::initializer_list<GLuint> shaders) {
    GLuint p = glCreateProgram();
    for (GLuint s : shaders) glAttachShader(p, s);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LUMINUMBRA_CORE_ERROR("ShieldRtFarFieldPass program link failed: {}", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}
GLuint Groups(int dim) { return static_cast<GLuint>((dim + 7) / 8); }

}  // namespace

ShieldRtFarFieldPass::ShieldRtFarFieldPass() = default;
ShieldRtFarFieldPass::~ShieldRtFarFieldPass() { shutdown(); }

bool ShieldRtFarFieldPass::init() {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kFullscreenVert);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kRaymarchFrag);
    GLuint c0 = CompileShader(GL_COMPUTE_SHADER, kMaxMipL0Compute);
    GLuint cr = CompileShader(GL_COMPUTE_SHADER, kMaxMipReduceCompute);
    if (vs && fs) m_raymarch_prog = LinkProgram({vs, fs});
    if (c0) m_maxmip_l0_prog = LinkProgram({c0});
    if (cr) m_maxmip_reduce_prog = LinkProgram({cr});
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (c0) glDeleteShader(c0);
    if (cr) glDeleteShader(cr);
    if (!m_raymarch_prog || !m_maxmip_l0_prog || !m_maxmip_reduce_prog) {
        shutdown();
        return false;
    }
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_base_ssbo);
    glGenBuffers(1, &m_maxmip_ssbo);
    m_ready = true;
    LUMINUMBRA_CORE_INFO("ShieldRtFarFieldPass initialized (experimental far-field raymarch)");
    return true;
}

void ShieldRtFarFieldPass::drain() {
    // Wait out an in-flight async heightfield build so its worker cannot read the
    // world after the caller tears it down (the job captures the world by pointer;
    // BuildPristineFarLodTile reads it). MUST be called before the world is cleared
    // / destroyed. m_shared is a shared_ptr so the hand-off buffer itself is safe.
    if (m_job_system && m_inflight_handle.counter) {
        m_job_system->wait(m_inflight_handle);
    }
    m_inflight_handle = JobHandle{};
    m_shared.reset();
    m_building = false;
}

void ShieldRtFarFieldPass::shutdown() {
    drain();
    if (m_raymarch_prog) { glDeleteProgram(m_raymarch_prog); m_raymarch_prog = 0; }
    if (m_maxmip_l0_prog) { glDeleteProgram(m_maxmip_l0_prog); m_maxmip_l0_prog = 0; }
    if (m_maxmip_reduce_prog) { glDeleteProgram(m_maxmip_reduce_prog); m_maxmip_reduce_prog = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_base_ssbo) { glDeleteBuffers(1, &m_base_ssbo); m_base_ssbo = 0; }
    if (m_maxmip_ssbo) { glDeleteBuffers(1, &m_maxmip_ssbo); m_maxmip_ssbo = 0; }
    if (m_scene_depth_fbo) { glDeleteFramebuffers(1, &m_scene_depth_fbo); m_scene_depth_fbo = 0; }
    if (m_scene_depth_tex) { glDeleteTextures(1, &m_scene_depth_tex); m_scene_depth_tex = 0; }
    m_depth_w = 0;
    m_depth_h = 0;
    m_have_depth_copy = false;
    m_ready = false;
    m_has_field = false;
    m_have_cache_key = false;
}

void ShieldRtFarFieldPass::capture_scene_depth(GLuint src_fbo, int width, int height) {
    if (!m_ready || width <= 0 || height <= 0) { m_have_depth_copy = false; return; }
    // Lazily (re)allocate the copy texture + FBO to the current viewport. Matches the
    // G-buffer depth format (DEPTH_COMPONENT24) so glBlitFramebuffer is a straight copy.
    if (m_scene_depth_tex == 0 || width != m_depth_w || height != m_depth_h) {
        if (m_scene_depth_tex == 0) glGenTextures(1, &m_scene_depth_tex);
        glBindTexture(GL_TEXTURE_2D, m_scene_depth_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (m_scene_depth_fbo == 0) glGenFramebuffers(1, &m_scene_depth_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_scene_depth_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_scene_depth_tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_depth_w = width;
        m_depth_h = height;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_depth_fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    m_have_depth_copy = true;
}

void ShieldRtFarFieldPass::update(const Systems::SHIELD_WorldSystem& world,
                                  const glm::vec3& camera_pos) {
    if (!m_ready) return;
    const int center_rx = static_cast<int>(std::floor(camera_pos.x / 512.0f));
    const int center_rz = static_cast<int>(std::floor(camera_pos.z / 512.0f));
    const std::uint64_t params_hash =
        World::ComputeTerrainParamsHash(world.get_params(), world.get_seed());

    // 1. Integrate a finished async build (GL upload + mip reduction on this thread).
    if (m_building && m_shared) {
        FieldData done;
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(m_shared->mutex);
            if (m_shared->ready) {
                done = std::move(m_shared->data);
                m_shared->ready = false;
                got = true;
            }
        }
        if (got) {
            integrate_field(done);  // sets m_has_field + the cache key to done's region
            m_building = false;
            m_shared.reset();
            m_inflight_handle = JobHandle{};
        }
    }

    // 2. Resident field already matches the camera region + params: nothing to do.
    if (m_have_cache_key && center_rx == m_center_rx && center_rz == m_center_rz &&
        params_hash == m_params_hash) {
        return;
    }

    // 3. A build for THIS exact target is already in flight: keep rendering the prior
    //    field until it lands (no re-dispatch).
    if (m_building && center_rx == m_inflight_rx && center_rz == m_inflight_rz &&
        params_hash == m_inflight_params) {
        return;
    }

    // 4a. No JobSystem (tests / headless without a pool): synchronous in-line build,
    //     preserving the validated inc2c behaviour.
    if (!m_job_system) {
        FieldData fd = assemble_field(world, center_rx, center_rz, params_hash);
        integrate_field(fd);
        return;
    }

    // 4b. A build for a now-stale target is still running: let it finish and integrate
    //     first (eventual consistency — the next update() dispatches the newest target).
    if (m_building) return;

    // 4c. Dispatch the async heightfield assembly on a worker; the prior field keeps
    //     rendering until update() integrates this one.
    m_shared = std::make_shared<FieldBuild>();
    m_inflight_rx = center_rx;
    m_inflight_rz = center_rz;
    m_inflight_params = params_hash;
    m_building = true;
    auto shared = m_shared;
    const Systems::SHIELD_WorldSystem* world_ptr = &world;
    m_inflight_handle = m_job_system->dispatch_batch(
        {[shared, world_ptr, center_rx, center_rz, params_hash]() {
            FieldData fd = assemble_field(*world_ptr, center_rx, center_rz, params_hash);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->data = std::move(fd);
            shared->ready = true;
        }},
        JobPriority::Normal);
}

ShieldRtFarFieldPass::FieldData ShieldRtFarFieldPass::assemble_field(
        const Systems::SHIELD_WorldSystem& world, int center_rx, int center_rz,
        std::uint64_t params_hash) {
    using World::FarLodTier;
    const int step = World::FarLodSampleStepMeters(FarLodTier::F1);  // 4 m
    const int per_region = 512 / step;                              // 128
    const int regions = 2 * kRegionRadius + 1;                      // 7
    const int rx0 = center_rx - kRegionRadius;
    const int rz0 = center_rz - kRegionRadius;

    FieldData fd;
    fd.center_rx = center_rx;
    fd.center_rz = center_rz;
    fd.params_hash = params_hash;
    fd.n = per_region * regions + 1;
    fd.step = static_cast<float>(step);
    fd.ox = static_cast<float>(rx0) * 512.0f;
    fd.oz = static_cast<float>(rz0) * 512.0f;

    fd.heights.assign(static_cast<std::size_t>(fd.n) * fd.n, 0.0f);
    for (int rz = 0; rz < regions; ++rz) {
        for (int rx = 0; rx < regions; ++rx) {
            const World::FarLodTile tile = World::BuildPristineFarLodTile(
                world, FarLodTier::F1, rx0 + rx, rz0 + rz, params_hash);
            const int sps = static_cast<int>(tile.samples_per_side);
            for (int z = 0; z < sps; ++z) {
                for (int x = 0; x < sps; ++x) {
                    const int gx = rx * per_region + x;
                    const int gz = rz * per_region + z;
                    if (gx >= fd.n || gz >= fd.n) continue;
                    fd.heights[static_cast<std::size_t>(gz) * fd.n + gx] =
                        World::DequantizeFarLodHeight(tile.height_q[z * sps + x]);
                }
            }
        }
    }
    return fd;
}

void ShieldRtFarFieldPass::integrate_field(FieldData& fd) {
    m_n = fd.n;
    m_step = fd.step;
    m_ox = fd.ox;
    m_oz = fd.oz;
    std::vector<float>& heights = fd.heights;

    // Flattened max-mip layout (dims match the CPU reference: level0 = n-1 cells,
    // then (dim+1)/2 each level to 1).
    int cells = m_n - 1;
    int off = 0;
    m_levels = 0;
    while (true) {
        m_mip_dim[m_levels] = cells;
        m_mip_offset[m_levels] = off;
        off += cells * cells;
        ++m_levels;
        if (cells == 1 || m_levels >= kMaxMipLevels) break;
        cells = (cells + 1) / 2;
    }

    // Upload base + size the max-mip buffer.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_base_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(heights.size() * sizeof(float)),
                 heights.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_maxmip_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(off) * sizeof(float),
                 nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Level 0 from base.
    glUseProgram(m_maxmip_l0_prog);
    glUniform1i(glGetUniformLocation(m_maxmip_l0_prog, "u_n"), m_n);
    glUniform1i(glGetUniformLocation(m_maxmip_l0_prog, "u_cells"), m_mip_dim[0]);
    glUniform1i(glGetUniformLocation(m_maxmip_l0_prog, "u_out_offset"), m_mip_offset[0]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_base_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_maxmip_ssbo);
    glDispatchCompute(Groups(m_mip_dim[0]), Groups(m_mip_dim[0]), 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // Coarser levels.
    glUseProgram(m_maxmip_reduce_prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_maxmip_ssbo);
    for (int Lv = 1; Lv < m_levels; ++Lv) {
        glUniform1i(glGetUniformLocation(m_maxmip_reduce_prog, "u_fine"), m_mip_dim[Lv - 1]);
        glUniform1i(glGetUniformLocation(m_maxmip_reduce_prog, "u_coarse"), m_mip_dim[Lv]);
        glUniform1i(glGetUniformLocation(m_maxmip_reduce_prog, "u_in_offset"), m_mip_offset[Lv - 1]);
        glUniform1i(glGetUniformLocation(m_maxmip_reduce_prog, "u_out_offset"), m_mip_offset[Lv]);
        glDispatchCompute(Groups(m_mip_dim[Lv]), Groups(m_mip_dim[Lv]), 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
    glUseProgram(0);
    m_has_field = true;
    // The resident field now matches this region/params (the async target may since
    // have moved on — update() re-dispatches if so).
    m_center_rx = fd.center_rx;
    m_center_rz = fd.center_rz;
    m_params_hash = fd.params_hash;
    m_have_cache_key = true;
}

void ShieldRtFarFieldPass::render(const glm::mat4& view, const glm::mat4& view_proj,
                                  const glm::mat4& inv_view_proj, const glm::mat3& normal_view,
                                  const glm::vec3& eye, const glm::vec2& viewport, float t_max) {
    if (!m_ready || !m_has_field) return;
    glUseProgram(m_raymarch_prog);
    glUniform2f(glGetUniformLocation(m_raymarch_prog, "u_viewport"), viewport.x, viewport.y);
    glUniform3f(glGetUniformLocation(m_raymarch_prog, "u_eye"), eye.x, eye.y, eye.z);
    glUniformMatrix4fv(glGetUniformLocation(m_raymarch_prog, "u_invViewProj"), 1, GL_FALSE, &inv_view_proj[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_raymarch_prog, "u_view"), 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_raymarch_prog, "u_viewProj"), 1, GL_FALSE, &view_proj[0][0]);
    glUniformMatrix3fv(glGetUniformLocation(m_raymarch_prog, "u_normalView"), 1, GL_FALSE, &normal_view[0][0]);
    glUniform1i(glGetUniformLocation(m_raymarch_prog, "u_n"), m_n);
    glUniform1f(glGetUniformLocation(m_raymarch_prog, "u_step"), m_step);
    glUniform2f(glGetUniformLocation(m_raymarch_prog, "u_origin"), m_ox, m_oz);
    glUniform1f(glGetUniformLocation(m_raymarch_prog, "u_tmax"), t_max);
    glUniform1i(glGetUniformLocation(m_raymarch_prog, "u_levels"), m_levels);
    glUniform1i(glGetUniformLocation(m_raymarch_prog, "u_maxSteps"), 512);
    glUniform1f(glGetUniformLocation(m_raymarch_prog, "u_seaLevel"), 0.0f);
    glUniform1iv(glGetUniformLocation(m_raymarch_prog, "u_mipOffset"), m_levels, m_mip_offset.data());
    glUniform1iv(glGetUniformLocation(m_raymarch_prog, "u_mipDim"), m_levels, m_mip_dim.data());
    // inc2c-SCALE step 1: bind the scene-depth copy + enable the far-pixel early-out
    // only when a fresh copy was captured this frame (else fall back to the validated
    // full-frame march so the pass is never wrong, just slower).
    const int early_out = m_have_depth_copy ? 1 : 0;
    glUniform1i(glGetUniformLocation(m_raymarch_prog, "u_farPixelEarlyOut"), early_out);
    if (early_out) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, m_scene_depth_tex);
        glUniform1i(glGetUniformLocation(m_raymarch_prog, "u_sceneDepth"), 8);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_base_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_maxmip_ssbo);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (early_out) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glUseProgram(0);
}

}  // namespace Luminumbra::Rendering
