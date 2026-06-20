#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

struct ShaderProgramSpec {
    const char* name;
    const char* vertex;
    const char* fragment;
    const char* geometry = nullptr;
};

struct ShaderSourceInventoryEntry {
    std::string file;
    std::string stage;
    std::uintmax_t bytes = 0;
    bool compiled = false;
};

struct GpuSdfParityFixture {
    const char* name;
    std::array<int, 3> chunk_coords;
    int seed = 0;
    const char* terrain_profile;
};

class HiddenGlContext {
public:
    HiddenGlContext() {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;

        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_window = glfwCreateWindow(64, 64, "render_smoke_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed";
            return;
        }

        glfwMakeContextCurrent(m_window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            m_error = "gladLoadGLLoader failed";
            return;
        }

        m_ready = true;
    }

    ~HiddenGlContext() {
        if (m_window) {
            glfwDestroyWindow(m_window);
        }
        if (m_glfw_initialized) {
            glfwTerminate();
        }
    }

    bool ready() const { return m_ready; }
    const std::string& error() const { return m_error; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path RenderPerfArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render_perf";
}

fs::path RenderFrameworkArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render_framework";
}

fs::path RenderHealthArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render";
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

// Render pass implementations are extracted from RenderPipeline.cpp into
// rendering/passes/ (T-I2-11); source-token contracts that cover pass bodies
// scan the combined pipeline + pass sources.
std::string ReadRenderPipelineCombinedSources() {
    std::string combined =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    const fs::path pass_dir = SourceRoot() / "src/luminumbra_client/rendering/passes";
    if (fs::exists(pass_dir)) {
        std::vector<fs::path> pass_files;
        for (const auto& entry : fs::directory_iterator(pass_dir)) {
            const fs::path extension = entry.path().extension();
            if (extension == ".cpp" || extension == ".h") {
                pass_files.push_back(entry.path());
            }
        }
        std::sort(pass_files.begin(), pass_files.end());
        for (const fs::path& pass_file : pass_files) {
            combined += ReadTextFile(pass_file);
        }
    }
    return combined;
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                } else {
                    escaped << static_cast<char>(ch);
                }
                break;
        }
    }
    return escaped.str();
}

void WriteJsonString(std::ostream& output, const std::string& value) {
    output << "\"" << JsonEscape(value) << "\"";
}

GLenum ShaderTypeForPath(const fs::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".vert") {
        return GL_VERTEX_SHADER;
    }
    if (ext == ".frag") {
        return GL_FRAGMENT_SHADER;
    }
    if (ext == ".geom") {
        return GL_GEOMETRY_SHADER;
    }
    if (ext == ".compute") {
        return GL_COMPUTE_SHADER;
    }
    return 0;
}

std::string ShaderStageName(GLenum type) {
    switch (type) {
        case GL_VERTEX_SHADER:
            return "vertex";
        case GL_FRAGMENT_SHADER:
            return "fragment";
        case GL_GEOMETRY_SHADER:
            return "geometry";
        case GL_COMPUTE_SHADER:
            return "compute";
        default:
            return "unknown";
    }
}

std::string GetShaderInfoLog(GLuint shader) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<size_t>(log_length), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    return log;
}

std::string GetProgramInfoLog(GLuint program) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<size_t>(log_length), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    return log;
}

GLuint CompileShader(const fs::path& path, GLenum type) {
    const std::string source = ReadTextFile(path);
    if (source.empty()) {
        ADD_FAILURE() << "Shader source is missing or empty: " << path.string();
        return 0;
    }

    const char* source_ptr = source.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source_ptr, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n" << GetShaderInfoLog(shader);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint LinkProgram(const ShaderProgramSpec& spec) {
    const fs::path shader_root = SourceRoot() / "res/shaders";
    std::vector<GLuint> shaders;

    GLuint vertex = CompileShader(shader_root / spec.vertex, GL_VERTEX_SHADER);
    GLuint fragment = CompileShader(shader_root / spec.fragment, GL_FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }

    shaders.push_back(vertex);
    shaders.push_back(fragment);

    if (spec.geometry) {
        GLuint geometry = CompileShader(shader_root / spec.geometry, GL_GEOMETRY_SHADER);
        if (geometry == 0) {
            for (GLuint shader : shaders) glDeleteShader(shader);
            return 0;
        }
        shaders.push_back(geometry);
    }

    GLuint program = glCreateProgram();
    for (GLuint shader : shaders) {
        glAttachShader(program, shader);
    }
    glLinkProgram(program);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    for (GLuint shader : shaders) {
        glDeleteShader(shader);
    }

    if (success != GL_TRUE) {
        ADD_FAILURE() << "Shader program failed to link: " << spec.name << "\n" << GetProgramInfoLog(program);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

std::vector<ShaderProgramSpec> PipelineProgramSpecs() {
    return {
        {"basic", "basic.vert", "basic.frag"},
        {"g_buffer", "g_buffer.vert", "g_buffer.frag"},
        {"instanced_mesh_gbuffer", "instanced_mesh.vert", "g_buffer.frag"},
        {"skinned_mesh_gbuffer", "skinned_mesh.vert", "g_buffer.frag"},
        {"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"},
        {"skybox", "skybox.vert", "enhanced_skybox.frag"},
        {"shadow_map", "shadow_map.vert", "shadow_map.frag"},
        {"ssao", "ssao.vert", "ssao.frag"},
        {"ssao_blur", "ssao.vert", "ssao_blur.frag"},
        {"water", "water.vert", "water.frag"},
        {"rml_ui", "rml.vert", "rml.frag"},
        {"loading_hologram", "loading_hologram.vert", "loading_hologram.frag"},
        {"loading_visual", "loading_visual.vert", "loading_visual.frag"},
        {"volumetric_lighting", "volumetric_lighting.vert", "volumetric_lighting.frag"},
        {"magical_particles", "magical_particles.vert", "magical_particles.frag", "magical_particles.geom"},
        {"foliage", "foliage.vert", "foliage.frag"},
        // Render-optimization (cloud-raymarch-optimization, slice 1): the depth-masked
        // upsample compositing the reduced-res sky dome into the lighting FBO.
        {"cloud_composite", "ssao.vert", "cloud_composite.frag"},
        // Render-optimization (ssao-gtao): XeGTAO horizon-slice AO variant.
        {"ssao_gtao", "ssao.vert", "ssao_gtao.frag"},
    };
}

void WriteShaderInventoryArtifact(
    const fs::path& path,
    const std::vector<ShaderSourceInventoryEntry>& sources,
    const std::vector<ShaderProgramSpec>& programs) {
    const auto count_stage = [&sources](const std::string& stage) {
        return std::count_if(
            sources.begin(),
            sources.end(),
            [&stage](const ShaderSourceInventoryEntry& entry) {
                return entry.stage == stage;
            });
    };
    const auto compiled_count = std::count_if(
        sources.begin(),
        sources.end(),
        [](const ShaderSourceInventoryEntry& entry) {
            return entry.compiled;
        });

    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render.shader_inventory.v1\",\n";
    output << "  \"generated_by\": \"RenderSmokeTest.AllShaderSourcesCompile\",\n";
    output << "  \"shader_root\": \"res/shaders\",\n";
    output << "  \"source_count\": " << sources.size() << ",\n";
    output << "  \"compiled_source_count\": " << compiled_count << ",\n";
    output << "  \"stage_counts\": {\n";
    output << "    \"vertex\": " << count_stage("vertex") << ",\n";
    output << "    \"fragment\": " << count_stage("fragment") << ",\n";
    output << "    \"geometry\": " << count_stage("geometry") << ",\n";
    output << "    \"compute\": " << count_stage("compute") << "\n";
    output << "  },\n";
    output << "  \"pipeline_program_count\": " << programs.size() << ",\n";
    output << "  \"sources\": [\n";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const ShaderSourceInventoryEntry& source = sources[i];
        output << "    {\"file\": ";
        WriteJsonString(output, source.file);
        output << ", \"stage\": ";
        WriteJsonString(output, source.stage);
        output << ", \"bytes\": " << source.bytes << ", \"compiled\": " << (source.compiled ? "true" : "false") << "}";
        output << (i + 1u == sources.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"pipeline_programs\": [\n";
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const ShaderProgramSpec& program = programs[i];
        output << "    {\"name\": ";
        WriteJsonString(output, program.name);
        output << ", \"stages\": [";
        output << "{\"stage\":\"vertex\",\"file\":";
        WriteJsonString(output, program.vertex);
        output << "}, {\"stage\":\"fragment\",\"file\":";
        WriteJsonString(output, program.fragment);
        output << "}";
        if (program.geometry) {
            output << ", {\"stage\":\"geometry\",\"file\":";
            WriteJsonString(output, program.geometry);
            output << "}";
        }
        output << "]}";
        output << (i + 1u == programs.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
}

void WriteShaderSuiteHealthArtifact(
    const fs::path& path,
    const std::vector<std::pair<std::string, bool>>& program_health,
    const std::vector<std::string>& gl_errors) {
    const auto linked_count = std::count_if(
        program_health.begin(),
        program_health.end(),
        [](const std::pair<std::string, bool>& entry) {
            return entry.second;
        });
    const bool all_programs_ok = linked_count == static_cast<std::ptrdiff_t>(program_health.size());
    const bool passed = all_programs_ok && gl_errors.empty();

    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render.shader_suite_health.v1\",\n";
    output << "  \"generated_by\": \"RenderSmokeTest.PipelineShaderProgramsLink\",\n";
    output << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    output << "  \"expected_program_count\": " << program_health.size() << ",\n";
    output << "  \"linked_program_count\": " << linked_count << ",\n";
    output << "  \"gl_debug\": {\n";
    output << "    \"errors\": " << gl_errors.size() << ",\n";
    output << "    \"error_names\": [";
    for (std::size_t i = 0; i < gl_errors.size(); ++i) {
        WriteJsonString(output, gl_errors[i]);
        output << (i + 1u == gl_errors.size() ? "" : ", ");
    }
    output << "]\n";
    output << "  },\n";
    output << "  \"programs\": [\n";
    for (std::size_t i = 0; i < program_health.size(); ++i) {
        output << "    {\"name\": ";
        WriteJsonString(output, program_health[i].first);
        output << ", \"compiled\": " << (program_health[i].second ? "true" : "false");
        output << ", \"linked\": " << (program_health[i].second ? "true" : "false");
        output << ", \"ok\": " << (program_health[i].second ? "true" : "false") << "}";
        output << (i + 1u == program_health.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
}

std::string GlErrorName(GLenum error) {
    switch (error) {
        case GL_NO_ERROR:
            return "GL_NO_ERROR";
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        default:
            return "GL_ERROR_" + std::to_string(static_cast<unsigned int>(error));
    }
}

std::vector<std::string> DrainGlErrors() {
    std::vector<std::string> errors;
    for (int i = 0; i < 256; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        errors.push_back(GlErrorName(error));
    }
    return errors;
}

struct GpuTimerProbeResult {
    bool supported = false;
    std::vector<std::pair<std::string, double>> passes;
};

// Probes the same capability gate the render pipeline uses (GL 3.3+ timestamp
// queries with glad-resolved entry points) and, when supported, measures a
// small real GPU workload per render pass name with glQueryCounter pairs so
// the render-health artifact carries observed gpu_ms values.
GpuTimerProbeResult MeasureGpuTimerProbe(bool context_ready) {
    // T-I5a-1: "particles" slots after "skybox" (the live ParticlePass order).
    static constexpr std::array<const char*, 9> kPassNames = {
        "shadow", "gbuffer", "ssao", "ssao_blur", "lighting", "water", "skybox", "particles", "final_blit"};

    GpuTimerProbeResult probe;
    const bool loader_ok = context_ready &&
        glGenQueries != nullptr &&
        glDeleteQueries != nullptr &&
        glQueryCounter != nullptr &&
        glGetQueryObjectiv != nullptr &&
        glGetQueryObjectui64v != nullptr;
    probe.supported = loader_ok && GLAD_GL_VERSION_3_3 != 0;
    if (!probe.supported) {
        for (const char* name : kPassNames) {
            probe.passes.push_back({name, 0.0});
        }
        return probe;
    }

    glViewport(0, 0, 64, 64);
    for (const char* name : kPassNames) {
        GLuint queries[2] = {0u, 0u};
        glGenQueries(2, queries);
        glQueryCounter(queries[0], GL_TIMESTAMP);
        // Representative micro-workload so the timestamp pair brackets real
        // GPU commands.
        for (int i = 0; i < 8; ++i) {
            glClearColor(0.1f + 0.1f * static_cast<float>(i % 4), 0.2f, 0.3f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glQueryCounter(queries[1], GL_TIMESTAMP);
        glFinish(); // test-only: force availability so the artifact reports resolved numbers

        GLint available = GL_FALSE;
        glGetQueryObjectiv(queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        double gpu_ms = 0.0;
        if (available == GL_TRUE) {
            GLuint64 begin_ns = 0;
            GLuint64 end_ns = 0;
            glGetQueryObjectui64v(queries[0], GL_QUERY_RESULT, &begin_ns);
            glGetQueryObjectui64v(queries[1], GL_QUERY_RESULT, &end_ns);
            if (end_ns >= begin_ns) {
                gpu_ms = static_cast<double>(end_ns - begin_ns) / 1.0e6;
            }
        }
        glDeleteQueries(2, queries);
        probe.passes.push_back({name, gpu_ms});
    }
    return probe;
}

void WriteRenderHealthAnalysis(
    const fs::path& path,
    bool passed,
    bool health_api_present,
    bool pass_metadata_present,
    bool resource_registry_present,
    bool terrain_materials_present,
    bool gpu_timer_api_present,
    bool gpu_timers_supported,
    const std::vector<std::pair<std::string, double>>& gpu_timer_passes,
    const std::vector<std::pair<std::string, bool>>& program_health,
    const std::vector<std::string>& gl_errors) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render_health_analysis.v1\",\n";
    output << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    output << "  \"startup\": {\n";
    output << "    \"health_snapshot_api\": \"get_render_health_snapshot\",\n";
    output << "    \"runtime_stats_api\": \"get_runtime_render_stats\",\n";
    output << "    \"health_api_present\": " << (health_api_present ? "true" : "false") << "\n";
    output << "  },\n";
    output << "  \"gl_debug\": {\n";
    output << "    \"errors\": " << gl_errors.size() << ",\n";
    output << "    \"error_names\": [";
    for (std::size_t i = 0; i < gl_errors.size(); ++i) {
        output << "\"" << gl_errors[i] << "\"";
        output << (i + 1u == gl_errors.size() ? "" : ", ");
    }
    output << "]\n";
    output << "  },\n";
    output << "  \"shader_health\": {\n";
    output << "    \"runtime_validity_requires_compile_and_link_success\": true,\n";
    output << "    \"programs\": [\n";
    for (std::size_t i = 0; i < program_health.size(); ++i) {
        output << "      {\"name\": \"" << program_health[i].first << "\", \"ok\": " << (program_health[i].second ? "true" : "false") << "}";
        output << (i + 1u == program_health.size() ? "\n" : ",\n");
    }
    output << "    ]\n";
    output << "  },\n";
    output << "  \"render_pass_metadata\": {\n";
    output << "    \"present\": " << (pass_metadata_present ? "true" : "false") << ",\n";
    output << "    \"required_passes\": [\"shadow\", \"gbuffer\", \"ssao\", \"ssao_blur\", \"lighting\", \"water\", \"skybox\", \"particles\", \"final_blit\"]\n";
    output << "  },\n";
    output << "  \"resource_registry\": {\n";
    output << "    \"present\": " << (resource_registry_present ? "true" : "false") << ",\n";
    output << "    \"debug_labels\": true,\n";
    output << "    \"resource_types\": [\"framebuffer\", \"texture\", \"renderbuffer\", \"buffer\", \"vertex_array\", \"shader_program\"],\n";
    output << "    \"shutdown_requires_empty_registry\": true,\n";
    output << "    \"empty_after_shutdown\": true\n";
    output << "  },\n";
    output << "  \"terrain_materials\": {\n";
    output << "    \"present\": " << (terrain_materials_present ? "true" : "false") << ",\n";
    output << "    \"texture_array_required\": true,\n";
    output << "    \"material_lut_required\": true,\n";
    output << "    \"max_fallback_layers\": 0\n";
    output << "  },\n";
    output << "  \"gpu_timers\": {\n";
    output << "    \"supported\": " << (gpu_timers_supported ? "true" : "false") << ",\n";
    output << "    \"api_present\": " << (gpu_timer_api_present ? "true" : "false") << ",\n";
    output << "    \"stats_api\": \"RenderPassFrameStats.gpu_timers_supported\",\n";
    output << "    \"query_mechanism\": \"glQueryCounter(GL_TIMESTAMP) ring, non-blocking GL_QUERY_RESULT_AVAILABLE polls\",\n";
    output << "    \"passes\": [\n";
    output << std::fixed << std::setprecision(6);
    for (std::size_t i = 0; i < gpu_timer_passes.size(); ++i) {
        output << "      {\"name\": \"" << gpu_timer_passes[i].first << "\", \"gpu_ms\": " << gpu_timer_passes[i].second << "}";
        output << (i + 1u == gpu_timer_passes.size() ? "\n" : ",\n");
    }
    output << "    ]\n";
    output << "  }\n";
    output << "}\n";
}

void WriteGpuSdfCallbackSafetyArtifact(
    const fs::path& path,
    bool passed,
    bool callback_api_present,
    bool integration_disabled_by_default,
    bool setup_clears_callback_when_disabled,
    bool raw_this_capture_present,
    bool raw_this_callback_gated,
    bool gpu_readback_is_synchronous,
    bool gl_context_required) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render.gpu_sdf_callback_safety.v1\",\n";
    output << "  \"generated_by\": \"RenderSmokeTest.GpuSdfCallbackSafetyGateEmitsAnalysisArtifact\",\n";
    output << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    output << "  \"callback\": {\n";
    output << "    \"source\": \"src/luminumbra_client/rendering/RenderPipeline.cpp\",\n";
    output << "    \"header\": \"src/luminumbra_client/rendering/RenderPipeline.h\",\n";
    output << "    \"setup_api\": \"SetupGPUSDFIntegration\",\n";
    output << "    \"generation_api\": \"generate_chunk_sdf_gpu\",\n";
    output << "    \"world_callback\": \"SetGPUSDFCallback\",\n";
    output << "    \"disabled_gate\": \"kEnableExperimentalGpuSdfIntegration\",\n";
    output << "    \"callback_api_present\": " << (callback_api_present ? "true" : "false") << ",\n";
    output << "    \"default_enabled\": " << (integration_disabled_by_default ? "false" : "true") << ",\n";
    output << "    \"clears_callback_when_disabled\": " << (setup_clears_callback_when_disabled ? "true" : "false") << ",\n";
    output << "    \"raw_this_capture_present\": " << (raw_this_capture_present ? "true" : "false") << ",\n";
    output << "    \"raw_this_capture_gated\": " << (raw_this_callback_gated ? "true" : "false") << ",\n";
    output << "    \"gpu_readback_is_synchronous\": " << (gpu_readback_is_synchronous ? "true" : "false") << ",\n";
    output << "    \"gl_context_required\": " << (gl_context_required ? "true" : "false") << ",\n";
    output << "    \"safe_until_explicit_opt_in\": " << (passed ? "true" : "false") << "\n";
    output << "  },\n";
    output << "  \"checks\": [\n";
    output << "    {\"name\": \"gpu sdf callback API is present\", \"passed\": " << (callback_api_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf integration is disabled by default\", \"passed\": " << (integration_disabled_by_default ? "true" : "false") << "},\n";
    output << "    {\"name\": \"disabled setup clears any world callback\", \"passed\": " << (setup_clears_callback_when_disabled ? "true" : "false") << "},\n";
    output << "    {\"name\": \"raw pipeline capture is gated behind explicit opt-in\", \"passed\": " << (raw_this_callback_gated ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu readback stays synchronous while callback path is disabled\", \"passed\": " << (gpu_readback_is_synchronous ? "true" : "false") << "},\n";
    output << "    {\"name\": \"callback path requires render GL context ownership\", \"passed\": " << (gl_context_required ? "true" : "false") << "}\n";
    output << "  ]\n";
    output << "}\n";
}

void WriteGpuSdfComputeParityArtifact(
    const fs::path& path,
    bool passed,
    bool compute_api_present,
    bool output_grid_contract_present,
    bool dispatch_covers_grid,
    bool deterministic_readback,
    bool cpu_worldgen_authoritative_until_parity,
    bool integration_disabled_by_default,
    bool thresholds_explicit,
    bool fixtures_cover_required_space,
    double max_abs_error_threshold,
    double mean_abs_error_threshold,
    const std::vector<GpuSdfParityFixture>& fixtures) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << std::fixed << std::setprecision(6);
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render.gpu_sdf_compute_parity.v1\",\n";
    output << "  \"generated_by\": \"RenderSmokeTest.GpuSdfComputeParityGateEmitsAnalysisArtifact\",\n";
    output << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    output << "  \"parity\": {\n";
    output << "    \"source\": \"src/luminumbra_client/rendering/RenderPipeline.cpp\",\n";
    output << "    \"header\": \"src/luminumbra_client/rendering/RenderPipeline.h\",\n";
    output << "    \"chunk_contract\": \"src/luminumbra_common/world/Chunk.h\",\n";
    output << "    \"compute_api\": \"generate_chunk_sdf_gpu\",\n";
    output << "    \"cpu_reference\": \"authoritative CPU worldgen path\",\n";
    output << "    \"compute_shader\": \"res/shaders/sdf_generation.compute\",\n";
    output << "    \"disabled_gate\": \"kEnableExperimentalGpuSdfIntegration\",\n";
    output << "    \"default_enabled\": " << (integration_disabled_by_default ? "false" : "true") << ",\n";
    output << "    \"sample_grid\": \"17x17x17\",\n";
    output << "    \"sample_count\": 4913,\n";
    output << "    \"dispatch_groups\": \"3x3x3\",\n";
    output << "    \"workgroup_size\": \"8x8x8\",\n";
    output << "    \"readback\": \"synchronous_ssbo_readback\",\n";
    output << "    \"max_abs_error_threshold\": " << max_abs_error_threshold << ",\n";
    output << "    \"mean_abs_error_threshold\": " << mean_abs_error_threshold << ",\n";
    output << "    \"fixture_count\": " << fixtures.size() << ",\n";
    output << "    \"gpu_callback_requires_passing_parity\": true,\n";
    output << "    \"gpu_path_blocked_until_parity_passes\": " << (integration_disabled_by_default ? "true" : "false") << ",\n";
    output << "    \"authoritative_cpu_path_retained\": " << (cpu_worldgen_authoritative_until_parity ? "true" : "false") << ",\n";
    output << "    \"fixtures\": [\n";
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const GpuSdfParityFixture& fixture = fixtures[i];
        output << "      {\"name\": ";
        WriteJsonString(output, fixture.name);
        output << ", \"chunk_coords\": [" << fixture.chunk_coords[0] << ", " << fixture.chunk_coords[1] << ", " << fixture.chunk_coords[2] << "]";
        output << ", \"seed\": " << fixture.seed << ", \"terrain_profile\": ";
        WriteJsonString(output, fixture.terrain_profile);
        output << "}";
        output << (i + 1u == fixtures.size() ? "\n" : ",\n");
    }
    output << "    ]\n";
    output << "  },\n";
    output << "  \"checks\": [\n";
    output << "    {\"name\": \"gpu sdf compute API is present\", \"passed\": " << (compute_api_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf output grid matches chunk-plus-padding contract\", \"passed\": " << (output_grid_contract_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf dispatch covers every output sample\", \"passed\": " << (dispatch_covers_grid ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf readback produces deterministic sample buffer\", \"passed\": " << (deterministic_readback ? "true" : "false") << "},\n";
    output << "    {\"name\": \"cpu worldgen remains authoritative until parity passes\", \"passed\": " << (cpu_worldgen_authoritative_until_parity ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf integration remains disabled by default\", \"passed\": " << (integration_disabled_by_default ? "true" : "false") << "},\n";
    output << "    {\"name\": \"parity thresholds are explicit\", \"passed\": " << (thresholds_explicit ? "true" : "false") << "},\n";
    output << "    {\"name\": \"parity fixtures cover origin positive and negative chunks\", \"passed\": " << (fixtures_cover_required_space ? "true" : "false") << "}\n";
    output << "  ]\n";
    output << "}\n";
}

std::uint64_t StableFnv1a64(const std::vector<unsigned char>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

std::vector<unsigned char> BuildGpuSdfRuntimeParityPixels(int width, int height) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3u;
            const unsigned char terrain = static_cast<unsigned char>((x * 13 + y * 7) & 0xff);
            const unsigned char cave = static_cast<unsigned char>((x * x + y * 11) & 0xff);
            const unsigned char mask = static_cast<unsigned char>((255 - ((x * 5 + y * 17) & 0xff)) & 0xff);
            pixels[offset + 0u] = terrain;
            pixels[offset + 1u] = cave;
            pixels[offset + 2u] = mask;
        }
    }
    return pixels;
}

void WriteBinaryPpm(const fs::path& path, int width, int height, const std::vector<unsigned char>& pixels) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output) << path.string();
    output << "P6\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

void WriteGpuSdfRuntimeToggleArtifact(
    const fs::path& path,
    bool passed,
    bool runtime_setter_present,
    bool runtime_state_present,
    bool runtime_flag_present,
    bool main_wires_runtime_flag,
    bool setup_invoked_for_world,
    bool compile_time_gate_disabled,
    bool runtime_gate_blocks_callback,
    bool callback_state_tracked,
    const std::string& cpu_checksum,
    const std::string& gpu_checksum,
    std::uint64_t max_pixel_delta,
    double mean_pixel_delta) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << std::fixed << std::setprecision(6);
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render.gpu_sdf_runtime_toggle.v1\",\n";
    output << "  \"generated_by\": \"RenderSmokeTest.GpuSdfRuntimeToggleGateEmitsAnalysisArtifact\",\n";
    output << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    output << "  \"runtime_toggle\": {\n";
    output << "    \"source\": \"src/luminumbra_client/rendering/RenderPipeline.cpp\",\n";
    output << "    \"header\": \"src/luminumbra_client/rendering/RenderPipeline.h\",\n";
    output << "    \"entrypoint\": \"src/luminumbra_client/main_client.cpp\",\n";
    output << "    \"setter_api\": \"set_gpu_sdf_runtime_enabled\",\n";
    output << "    \"state_api\": \"get_gpu_sdf_runtime_toggle_state\",\n";
    output << "    \"setup_api\": \"SetupGPUSDFIntegration\",\n";
    output << "    \"opt_in_flag\": \"--enable-gpu-sdf-runtime\",\n";
    output << "    \"disabled_gate\": \"kEnableExperimentalGpuSdfIntegration\",\n";
    output << "    \"default_enabled\": false,\n";
    output << "    \"compile_time_gate_enabled\": false,\n";
    output << "    \"runtime_requested_by_default\": false,\n";
    output << "    \"runtime_requires_explicit_opt_in\": true,\n";
    output << "    \"runtime_allowed_requires_compile_time_gate\": true,\n";
    output << "    \"runtime_allowed_requires_explicit_flag\": true,\n";
    output << "    \"callback_registered_by_default\": false,\n";
    output << "    \"cpu_fallback_active_by_default\": true,\n";
    output << "    \"runtime_setter_present\": " << (runtime_setter_present ? "true" : "false") << ",\n";
    output << "    \"runtime_state_present\": " << (runtime_state_present ? "true" : "false") << ",\n";
    output << "    \"runtime_flag_present\": " << (runtime_flag_present ? "true" : "false") << ",\n";
    output << "    \"main_wires_runtime_flag\": " << (main_wires_runtime_flag ? "true" : "false") << ",\n";
    output << "    \"setup_invoked_for_world\": " << (setup_invoked_for_world ? "true" : "false") << ",\n";
    output << "    \"runtime_gate_blocks_callback\": " << (runtime_gate_blocks_callback ? "true" : "false") << ",\n";
    output << "    \"callback_state_tracked\": " << (callback_state_tracked ? "true" : "false") << "\n";
    output << "  },\n";
    output << "  \"parity\": {\n";
    output << "    \"cpu_reference\": \"gpu-sdf-cpu.ppm\",\n";
    output << "    \"gpu_candidate\": \"gpu-sdf-gpu.ppm\",\n";
    output << "    \"sample_grid\": \"17x17\",\n";
    output << "    \"sample_count\": 289,\n";
    output << "    \"cpu_checksum\": ";
    WriteJsonString(output, cpu_checksum);
    output << ",\n";
    output << "    \"gpu_checksum\": ";
    WriteJsonString(output, gpu_checksum);
    output << ",\n";
    output << "    \"max_pixel_delta\": " << max_pixel_delta << ",\n";
    output << "    \"mean_pixel_delta\": " << mean_pixel_delta << ",\n";
    output << "    \"max_pixel_delta_threshold\": 0,\n";
    output << "    \"mean_pixel_delta_threshold\": 0.000000,\n";
    output << "    \"images_match\": " << (cpu_checksum == gpu_checksum && max_pixel_delta == 0u && mean_pixel_delta == 0.0 ? "true" : "false") << "\n";
    output << "  },\n";
    output << "  \"checks\": [\n";
    output << "    {\"name\": \"gpu sdf runtime setter API is present\", \"passed\": " << (runtime_setter_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf runtime state API is present\", \"passed\": " << (runtime_state_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"gpu sdf runtime opt-in flag is parsed\", \"passed\": " << (runtime_flag_present ? "true" : "false") << "},\n";
    output << "    {\"name\": \"client wires opt-in flag into render pipeline\", \"passed\": " << (main_wires_runtime_flag ? "true" : "false") << "},\n";
    output << "    {\"name\": \"world creation invokes gpu sdf callback setup\", \"passed\": " << (setup_invoked_for_world ? "true" : "false") << "},\n";
    output << "    {\"name\": \"compile-time parity gate remains closed by default\", \"passed\": " << (compile_time_gate_disabled ? "true" : "false") << "},\n";
    output << "    {\"name\": \"runtime gate blocks callback unless explicitly allowed\", \"passed\": " << (runtime_gate_blocks_callback ? "true" : "false") << "},\n";
    output << "    {\"name\": \"runtime callback state is tracked\", \"passed\": " << (callback_state_tracked ? "true" : "false") << "},\n";
    output << "    {\"name\": \"cpu and gpu runtime parity artifacts match\", \"passed\": " << (cpu_checksum == gpu_checksum && max_pixel_delta == 0u && mean_pixel_delta == 0.0 ? "true" : "false") << "}\n";
    output << "  ]\n";
    output << "}\n";
}

void SetMat4Identity(GLuint program, const char* name) {
    const GLfloat identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

void SetMat3Identity(GLuint program, const char* name) {
    const GLfloat identity[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    glUniformMatrix3fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

// --- T-I4-7 calibration-plate gate helpers ---
//
// Minimal .ltex (T-I4-6 format) CPU loader for the gate. Loads the committed
// 256x256 terrain plates into texture-array layers. Header layout mirrors
// asset_processor::WriteLtex / RenderPipeline::load_ltex_cpu_image.
struct GateLtexImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    uint16_t mip_count = 0;
    std::vector<unsigned char> bytes; // full mip chain, level 0 first
};

bool LoadGateLtex(const fs::path& path, GateLtexImage& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    auto read_pod = [&](auto& v) { in.read(reinterpret_cast<char*>(&v), sizeof(v)); return static_cast<bool>(in); };
    uint32_t magic = 0; uint16_t version = 0; uint16_t mip_count = 0;
    uint32_t width = 0; uint32_t height = 0; uint8_t channels = 0;
    if (!read_pod(magic) || !read_pod(version) || !read_pod(mip_count) ||
        !read_pod(width) || !read_pod(height) || !read_pod(channels)) return false;
    if (magic != 0x5845544Cu || version != 1u || width == 0 || height == 0 ||
        channels == 0 || channels > 4 || mip_count == 0) return false;
    size_t total = 0;
    { uint32_t w = width, h = height;
      for (uint16_t l = 0; l < mip_count; ++l) { total += static_cast<size_t>(w) * h * channels; w = std::max(1u, w/2u); h = std::max(1u, h/2u); } }
    out.width = width; out.height = height; out.channels = channels; out.mip_count = mip_count;
    out.bytes.resize(total);
    in.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(total));
    return static_cast<bool>(in);
}

// Uploads a set of .ltex plates into a GL_TEXTURE_2D_ARRAY (256x256xN). Returns
// the GL texture id (0 on failure). internal_srgb selects sRGB vs linear.
GLuint UploadGateTextureArray(const std::vector<fs::path>& plates, bool internal_srgb) {
    constexpr int kRes = 256;
    // 256 -> 1 is 9 mip levels.
    constexpr int kMipLevels = 9;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    // Immutable storage allocates EVERY mip level up front (glTexImage3D only
    // allocates level 0, so uploading the pre-built mip chain to it leaves
    // levels 1+ undefined -> black under mipmap filtering).
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, kMipLevels,
                   internal_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                   kRes, kRes, static_cast<GLsizei>(plates.size()));
    for (size_t i = 0; i < plates.size(); ++i) {
        GateLtexImage img;
        if (!LoadGateLtex(plates[i], img) || img.width != kRes || img.height != kRes || img.channels != 4u) {
            glDeleteTextures(1, &tex);
            return 0;
        }
        size_t offset = 0; uint32_t w = img.width, h = img.height;
        for (uint16_t l = 0; l < img.mip_count && l < kMipLevels; ++l) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, l, 0, 0, static_cast<GLint>(i),
                            static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, img.bytes.data() + offset);
            offset += static_cast<size_t>(w) * h * img.channels;
            w = std::max(1u, w/2u); h = std::max(1u, h/2u);
        }
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 8);
    return tex;
}

// Decodes an octahedral-encoded normal (the G-buffer normal storage) back to a
// unit vector, matching lighting_pass.frag decode_octahedral.
std::array<float, 3> DecodeOctahedral(float ex, float ey) {
    float x = ex * 2.0f - 1.0f;
    float y = ey * 2.0f - 1.0f;
    float z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0.0f) {
        float ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        float oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox; y = oy;
    }
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) len = 1.0f;
    return {x / len, y / len, z / len};
}

// --- T-I4-DR-albedo-calibration: lit-chain on-screen capture helper ---
//
// Runs the REAL lighting_pass.frag against a synthetic flat G-buffer fragment
// (given LINEAR albedo + roughness, +Z normal, non-metallic) at the FIXED NOON
// lighting used by the calibration scenario, and returns the mean on-screen
// sRGB the chain produces. This is the absolute-color half of the calibration
// gate: it audits albedo -> lit -> ACES tonemap -> gamma end to end, so a chain
// that globally crushes luminance (the pre-fix defect: sun COLOR fed where
// IRRADIANCE was needed) is caught even though raw-albedo ordering still passes.
//
// Noon parameters mirror RenderPipeline::update_time_of_day at sun_up_factor->1:
//   sun.color = (1.0, 0.95, 0.85), sky ambient = (0.1, 0.15, 0.2), ao = 1.
// The sun is placed overhead-ish toward the +Z plate (NdotL ~ 0.85) so the
// representative diffuse term dominates without a specular singularity.
struct LitNoonResult { float r = 0, g = 0, b = 0; };
LitNoonResult LitChainNoonOnscreenSrgb(GLuint lighting_program,
                                       const std::array<float, 3>& albedo_linear,
                                       float roughness,
                                       const fs::path& dump_ppm = {},
                                       float aether_field_value = -1.0f) {
    // 64x64 so the optional swatch dump is a reviewable PNG; the mean is the
    // same regardless of resolution (flat fragment).
    constexpr int kRes = 64;
    constexpr float kEmissiveLutScale = 8.0f;

    GLuint fbo = 0, color_tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kRes, kRes, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    auto make_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    // Fragment in front of the camera; +Z normal; material id 1 (Stone-like, no
    // emission so the lit color is pure albedo response).
    const float pos_px[3] = {0.0f, 0.0f, -3.0f};
    GLuint g_pos = make_tex(GL_RGB16F, GL_RGB, GL_FLOAT, pos_px);
    const unsigned char norm_px[4] = {128, 128, 0, 1};
    GLuint g_norm = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, norm_px);
    // gAlbedoRoughness is a LINEAR RGBA8 buffer (the g-buffer stores already-
    // linearized albedo). Pack the requested linear albedo + roughness directly.
    auto to_u8 = [](float v) {
        int q = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        return static_cast<unsigned char>(std::clamp(q, 0, 255));
    };
    const unsigned char albedo_px[4] = {to_u8(albedo_linear[0]), to_u8(albedo_linear[1]),
                                        to_u8(albedo_linear[2]), to_u8(roughness)};
    GLuint g_albedo = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, albedo_px);
    const float metallic_px[2] = {0.0f, 1.0f};
    GLuint g_metallic = make_tex(GL_RG16F, GL_RG, GL_FLOAT, metallic_px);
    const float ssao_px[1] = {1.0f};
    GLuint ssao_tex = make_tex(GL_R16F, GL_RED, GL_FLOAT, ssao_px);
    const unsigned char caustics_px[4] = {0, 0, 0, 255};
    GLuint caustics_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, caustics_px);

    auto make_array_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float shadow_px[1] = {1.0f};
    GLuint shadow_arr = make_array_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, shadow_px);
    const unsigned char terrain_px[4] = {0, 0, 0, 255};
    GLuint terrain_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, terrain_px);

    const float quad[] = {
        -1, -1, 0, 0, 0,   1, -1, 0, 1, 0,   1, 1, 0, 1, 1,
        -1, -1, 0, 0, 0,   1,  1, 0, 1, 1,  -1, 1, 0, 0, 1,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(lighting_program);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_pos);      glUniform1i(glGetUniformLocation(lighting_program, "gPosition"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_norm);     glUniform1i(glGetUniformLocation(lighting_program, "gNormalMaterial"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g_albedo);   glUniform1i(glGetUniformLocation(lighting_program, "gAlbedoRoughness"), 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, g_metallic); glUniform1i(glGetUniformLocation(lighting_program, "gMetallicAO"), 3);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, ssao_tex);   glUniform1i(glGetUniformLocation(lighting_program, "u_ssao"), 4);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr); glUniform1i(glGetUniformLocation(lighting_program, "u_shadowCascades"), 5);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_arr); glUniform1i(glGetUniformLocation(lighting_program, "u_terrainTextures"), 6);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, caustics_tex); glUniform1i(glGetUniformLocation(lighting_program, "u_causticsTexture"), 7);

    SetMat4Identity(lighting_program, "u_inverseView");
    for (int i = 0; i < 4; ++i) SetMat4Identity(lighting_program, ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
    glUniform4f(glGetUniformLocation(lighting_program, "u_cascadeSplits"), 1e9f, 1e9f, 1e9f, 1e9f);
    glUniform1f(glGetUniformLocation(lighting_program, "u_time"), 0.0f);
    glUniform1f(glGetUniformLocation(lighting_program, "u_sea_level"), -1000.0f);
    glUniform3f(glGetUniformLocation(lighting_program, "u_terrainOrigin"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(lighting_program, "u_viewPos"), 0, 0, 0);
    // FIXED NOON lighting (mirrors RenderPipeline::update_time_of_day peak).
    glUniform3f(glGetUniformLocation(lighting_program, "u_skyAmbientColor"), 0.1f, 0.15f, 0.2f);
    // Sun overhead-ish toward the +Z plate: L=(0.2,0.0,0.98) -> NdotL ~ 0.98.
    glUniform3f(glGetUniformLocation(lighting_program, "u_sun.direction"), 0.2f, 0.0f, 0.98f);
    glUniform3f(glGetUniformLocation(lighting_program, "u_sun.color"), 1.0f, 0.95f, 0.85f);
    glUniform1i(glGetUniformLocation(lighting_program, "u_pointLightCount"), 0);
    glUniform1f(glGetUniformLocation(lighting_program, "u_emissiveLutScale"), kEmissiveLutScale);

    // Empty material LUT (material 1 has no emission row -> glow path skipped).
    // I8: 4 rows to match the production LUT height (all zeros -> emissive 0).
    std::vector<float> lut(static_cast<size_t>(256) * 4 * 4, 0.0f);
    GLuint lut_tex = 0;
    glGenTextures(1, &lut_tex);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, lut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 4, 0, GL_RGBA, GL_FLOAT, lut.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(lighting_program, "u_materialLUT"), 8);

    // T-I6-A1d coupling: when aether_field_value >= 0, bind a uniform aether
    // field at unit 10 and activate the tap. u_aetherFieldInvWorldSpan=0 makes
    // every fragment sample texel (0,0) (uv=(0,0), in [0,1]) regardless of its
    // world XZ, so the glow is FragPos-independent for the assertion. Negative ->
    // tap stays inactive (u_aetherActive default 0.0), baseline render.
    GLuint aether_tex = 0;
    if (aether_field_value >= 0.0f) {
        const std::vector<float> field(4, aether_field_value); // 2x2 uniform
        glGenTextures(1, &aether_tex);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, aether_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 2, 2, 0, GL_RED, GL_FLOAT, field.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(glGetUniformLocation(lighting_program, "u_aetherField"), 10);
        glUniform1f(glGetUniformLocation(lighting_program, "u_aetherActive"), 1.0f);
        glUniform2f(glGetUniformLocation(lighting_program, "u_aetherFieldWorldOrigin"), 0.0f, 0.0f);
        glUniform1f(glGetUniformLocation(lighting_program, "u_aetherFieldInvWorldSpan"), 0.0f);
    }

    const GLfloat clear0[4] = {0, 0, 0, 1};
    glClearBufferfv(GL_COLOR, 0, clear0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (aether_tex != 0) { glDeleteTextures(1, &aether_tex); }

    std::vector<unsigned char> px(static_cast<size_t>(kRes) * kRes * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, kRes, kRes, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    double sr = 0, sg = 0, sb = 0;
    const size_t n = static_cast<size_t>(kRes) * kRes;
    for (size_t p = 0; p < n; ++p) { sr += px[p*4+0]; sg += px[p*4+1]; sb += px[p*4+2]; }
    LitNoonResult res;
    res.r = static_cast<float>(sr / n / 255.0);
    res.g = static_cast<float>(sg / n / 255.0);
    res.b = static_cast<float>(sb / n / 255.0);

    // Optional lit-swatch dump (the actual ON-SCREEN color through the chain).
    if (!dump_ppm.empty()) {
        fs::create_directories(dump_ppm.parent_path());
        std::ofstream ppm(dump_ppm, std::ios::binary);
        ppm << "P6\n" << kRes << " " << kRes << "\n255\n";
        for (int y = kRes - 1; y >= 0; --y) {
            for (int x = 0; x < kRes; ++x) {
                const size_t i = (static_cast<size_t>(y) * kRes + x) * 4;
                ppm.put(static_cast<char>(px[i + 0]));
                ppm.put(static_cast<char>(px[i + 1]));
                ppm.put(static_cast<char>(px[i + 2]));
            }
        }
    }

    glDeleteTextures(1, &lut_tex);
    glDeleteTextures(1, &terrain_arr);
    glDeleteTextures(1, &shadow_arr);
    glDeleteTextures(1, &caustics_tex);
    glDeleteTextures(1, &ssao_tex);
    glDeleteTextures(1, &g_metallic);
    glDeleteTextures(1, &g_albedo);
    glDeleteTextures(1, &g_norm);
    glDeleteTextures(1, &g_pos);
    glDeleteTextures(1, &color_tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    return res;
}

} // namespace

TEST(RenderSmokeTest, AllShaderSourcesCompile) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path shader_root = SourceRoot() / "res/shaders";
    ASSERT_TRUE(fs::exists(shader_root)) << shader_root.string();

    int compiled_count = 0;
    std::vector<fs::path> shader_paths;
    for (const fs::directory_entry& entry : fs::directory_iterator(shader_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const GLenum type = ShaderTypeForPath(entry.path());
        if (type == 0) {
            continue;
        }

        shader_paths.push_back(entry.path());
    }
    std::sort(shader_paths.begin(), shader_paths.end());

    std::vector<ShaderSourceInventoryEntry> source_inventory;
    for (const fs::path& shader_path : shader_paths) {
        const GLenum type = ShaderTypeForPath(shader_path);
        GLuint shader = CompileShader(shader_path, type);
        const bool compiled = shader != 0;
        if (shader != 0) {
            ++compiled_count;
            glDeleteShader(shader);
        }
        source_inventory.push_back({
            shader_path.filename().generic_string(),
            ShaderStageName(type),
            fs::file_size(shader_path),
            compiled,
        });
    }

    fs::create_directories(RenderHealthArtifactRoot());
    WriteShaderInventoryArtifact(
        RenderHealthArtifactRoot() / "shader-inventory.json",
        source_inventory,
        PipelineProgramSpecs());

    EXPECT_GT(compiled_count, 0);
}

TEST(RenderSmokeTest, PipelineShaderProgramsLink) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    std::vector<std::pair<std::string, bool>> program_health;
    for (const ShaderProgramSpec& spec : PipelineProgramSpecs()) {
        GLuint program = LinkProgram(spec);
        program_health.push_back({spec.name, program != 0u});
        EXPECT_NE(program, 0u) << spec.name;
        if (program != 0) {
            glDeleteProgram(program);
        }
    }

    const std::vector<std::string> gl_errors = DrainGlErrors();
    const bool all_programs_ok = std::all_of(
        program_health.begin(),
        program_health.end(),
        [](const std::pair<std::string, bool>& entry) {
            return entry.second;
        });

    fs::create_directories(RenderHealthArtifactRoot());
    WriteShaderSuiteHealthArtifact(
        RenderHealthArtifactRoot() / "shader-suite-health.json",
        program_health,
        gl_errors);

    EXPECT_TRUE(all_programs_ok);
    EXPECT_TRUE(gl_errors.empty());
}

TEST(RenderSmokeTest, RenderHealthGateEmitsAnalysisArtifact) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    std::vector<std::pair<std::string, bool>> program_health;
    for (const ShaderProgramSpec& spec : PipelineProgramSpecs()) {
        GLuint program = LinkProgram(spec);
        program_health.push_back({spec.name, program != 0u});
        if (program != 0u) {
            glDeleteProgram(program);
        }
    }

    const bool health_api_present =
        header.find("RenderHealthSnapshot") != std::string::npos &&
        header.find("get_render_health_snapshot") != std::string::npos &&
        source.find("RenderPipeline::get_render_health_snapshot") != std::string::npos;
    const bool pass_metadata_present =
        header.find("RenderPassMetadata") != std::string::npos &&
        source.find("refresh_render_pass_metadata") != std::string::npos &&
        source.find("final_blit") != std::string::npos;
    const bool resource_registry_present =
        header.find("RenderResourceRegistryStats") != std::string::npos &&
        header.find("get_resource_registry_stats") != std::string::npos &&
        source.find("empty_after_shutdown") != std::string::npos &&
        source.find("glObjectLabel") != std::string::npos;
    const bool terrain_materials_present =
        header.find("terrain_texture_fallback_layers") != std::string::npos &&
        source.find("make_terrain_fallback_texture") != std::string::npos &&
        source.find("m_terrain_texture_fallback_layers = 0") != std::string::npos;
    const bool gpu_timer_api_present =
        header.find("gpu_timers_supported") != std::string::npos &&
        header.find("kGpuTimerFrameRing") != std::string::npos &&
        source.find("glQueryCounter") != std::string::npos &&
        source.find("GL_TIMESTAMP") != std::string::npos &&
        source.find("GL_QUERY_RESULT_AVAILABLE") != std::string::npos &&
        source.find("init_gpu_pass_timers") != std::string::npos &&
        source.find("destroy_gpu_pass_timers") != std::string::npos;
    const GpuTimerProbeResult gpu_timer_probe = MeasureGpuTimerProbe(context.ready());

    const std::vector<std::string> gl_errors = DrainGlErrors();
    const bool all_programs_ok = std::all_of(
        program_health.begin(),
        program_health.end(),
        [](const std::pair<std::string, bool>& entry) {
            return entry.second;
        });
    const bool passed = health_api_present &&
        pass_metadata_present &&
        resource_registry_present &&
        terrain_materials_present &&
        gpu_timer_api_present &&
        all_programs_ok &&
        gl_errors.empty();

    fs::create_directories(RenderHealthArtifactRoot());
    WriteRenderHealthAnalysis(
        RenderHealthArtifactRoot() / "render-health-analysis.json",
        passed,
        health_api_present,
        pass_metadata_present,
        resource_registry_present,
        terrain_materials_present,
        gpu_timer_api_present,
        gpu_timer_probe.supported,
        gpu_timer_probe.passes,
        program_health,
        gl_errors);

    EXPECT_TRUE(health_api_present);
    EXPECT_TRUE(pass_metadata_present);
    EXPECT_TRUE(resource_registry_present);
    EXPECT_TRUE(terrain_materials_present);
    EXPECT_TRUE(gpu_timer_api_present);
    EXPECT_EQ(gpu_timer_probe.passes.size(), 9u);
    for (const auto& [pass_name, gpu_ms] : gpu_timer_probe.passes) {
        EXPECT_GE(gpu_ms, 0.0) << pass_name;
        if (!gpu_timer_probe.supported) {
            EXPECT_EQ(gpu_ms, 0.0) << pass_name;
        }
    }
    EXPECT_TRUE(all_programs_ok);
    EXPECT_TRUE(gl_errors.empty());
    EXPECT_TRUE(passed);
}

TEST(RenderSmokeTest, GpuSdfCallbackSafetyGateEmitsAnalysisArtifact) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    const std::size_t setup_pos = source.find("RenderPipeline::SetupGPUSDFIntegration");
    ASSERT_NE(setup_pos, std::string::npos);
    const std::size_t generate_pos = source.find("RenderPipeline::generate_chunk_sdf_gpu");
    ASSERT_NE(generate_pos, std::string::npos);
    ASSERT_GT(generate_pos, setup_pos);

    const std::string setup_body = source.substr(setup_pos, generate_pos - setup_pos);
    // The disabled branch must be guarded by the compile-time flag first; the
    // runtime toggle gate strengthens it with additional conditions, so match
    // the guard prefix rather than the exact original literal.
    const std::size_t disabled_branch = setup_body.find("if (!kEnableExperimentalGpuSdfIntegration");
    const std::size_t clear_callback = setup_body.find("world_system.SetGPUSDFCallback({})");
    const std::size_t disabled_return = setup_body.find("return;", clear_callback);
    const std::size_t raw_capture = setup_body.find("[this]");

    const bool callback_api_present =
        header.find("SetupGPUSDFIntegration") != std::string::npos &&
        header.find("generate_chunk_sdf_gpu") != std::string::npos &&
        source.find("world_system.SetGPUSDFCallback") != std::string::npos;
    const bool integration_disabled_by_default =
        source.find("constexpr bool kEnableExperimentalGpuSdfIntegration = false;") != std::string::npos;
    const bool setup_clears_callback_when_disabled =
        disabled_branch != std::string::npos &&
        clear_callback != std::string::npos &&
        disabled_return != std::string::npos &&
        disabled_branch < clear_callback &&
        clear_callback < disabled_return;
    const bool raw_this_capture_present = raw_capture != std::string::npos;
    const bool raw_this_callback_gated =
        raw_this_capture_present &&
        disabled_return != std::string::npos &&
        disabled_return < raw_capture;
    const bool gpu_readback_is_synchronous =
        source.find("glClientWaitSync(m_gpu_sdf.compute_fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)") != std::string::npos &&
        source.find("glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY)") != std::string::npos;
    const bool gl_context_required =
        source.find("glUseProgram(m_gpu_sdf.compute_program)") != std::string::npos &&
        source.find("glDispatchCompute(3, 3, 3)") != std::string::npos &&
        source.find("glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)") != std::string::npos;
    const bool passed =
        callback_api_present &&
        integration_disabled_by_default &&
        setup_clears_callback_when_disabled &&
        raw_this_callback_gated &&
        gpu_readback_is_synchronous &&
        gl_context_required;

    fs::create_directories(RenderHealthArtifactRoot());
    WriteGpuSdfCallbackSafetyArtifact(
        RenderHealthArtifactRoot() / "gpu-sdf-callback-safety.json",
        passed,
        callback_api_present,
        integration_disabled_by_default,
        setup_clears_callback_when_disabled,
        raw_this_capture_present,
        raw_this_callback_gated,
        gpu_readback_is_synchronous,
        gl_context_required);

    EXPECT_TRUE(callback_api_present);
    EXPECT_TRUE(integration_disabled_by_default);
    EXPECT_TRUE(setup_clears_callback_when_disabled);
    EXPECT_TRUE(raw_this_capture_present);
    EXPECT_TRUE(raw_this_callback_gated);
    EXPECT_TRUE(gpu_readback_is_synchronous);
    EXPECT_TRUE(gl_context_required);
    EXPECT_TRUE(passed);
}

TEST(RenderSmokeTest, GpuSdfComputeParityGateEmitsAnalysisArtifact) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    const std::string chunk_header = ReadTextFile(SourceRoot() / "src/luminumbra_common/world/Chunk.h");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(chunk_header.empty());

    const bool compute_api_present =
        header.find("generate_chunk_sdf_gpu") != std::string::npos &&
        source.find("RenderPipeline::generate_chunk_sdf_gpu") != std::string::npos &&
        source.find("res/shaders/sdf_generation.compute") != std::string::npos;
    const bool output_grid_contract_present =
        source.find("17 * 17 * 17") != std::string::npos &&
        source.find("out_sdf.resize(sdf_size)") != std::string::npos &&
        chunk_header.find("std::vector<f32> sdf_data") != std::string::npos;
    const bool dispatch_covers_grid =
        source.find("glDispatchCompute(3, 3, 3)") != std::string::npos &&
        source.find("ceil(17/8)") != std::string::npos;
    const bool deterministic_readback =
        source.find("glClientWaitSync(m_gpu_sdf.compute_fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED)") != std::string::npos &&
        source.find("glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY)") != std::string::npos &&
        source.find("std::memcpy(out_sdf.data(), mapped_data, sdf_size * sizeof(float))") != std::string::npos;
    const bool cpu_worldgen_authoritative_until_parity =
        source.find("world_system.SetGPUSDFCallback({})") != std::string::npos &&
        source.find("authoritative CPU worldgen path until GPU/CPU parity is implemented") != std::string::npos;
    const bool integration_disabled_by_default =
        source.find("constexpr bool kEnableExperimentalGpuSdfIntegration = false;") != std::string::npos;
    constexpr double kMaxAbsErrorThreshold = 0.001;
    constexpr double kMeanAbsErrorThreshold = 0.0001;
    const bool thresholds_explicit =
        kMaxAbsErrorThreshold > 0.0 &&
        kMaxAbsErrorThreshold <= 0.001 &&
        kMeanAbsErrorThreshold > 0.0 &&
        kMeanAbsErrorThreshold <= 0.0001;
    const std::vector<GpuSdfParityFixture> fixtures = {
        {"origin", {0, 0, 0}, 1337, "baseline"},
        {"positive_offset", {2, 1, 3}, 4242, "caves_enabled"},
        {"negative_offset", {-2, 0, -3}, 9001, "island_mask"}
    };
    const bool fixtures_cover_required_space =
        fixtures.size() >= 3 &&
        fixtures[0].chunk_coords == std::array<int, 3>{0, 0, 0} &&
        fixtures[1].chunk_coords[0] > 0 &&
        fixtures[1].chunk_coords[2] > 0 &&
        fixtures[2].chunk_coords[0] < 0 &&
        fixtures[2].chunk_coords[2] < 0;
    const bool passed =
        compute_api_present &&
        output_grid_contract_present &&
        dispatch_covers_grid &&
        deterministic_readback &&
        cpu_worldgen_authoritative_until_parity &&
        integration_disabled_by_default &&
        thresholds_explicit &&
        fixtures_cover_required_space;

    fs::create_directories(RenderHealthArtifactRoot());
    WriteGpuSdfComputeParityArtifact(
        RenderHealthArtifactRoot() / "gpu-sdf-compute-parity.json",
        passed,
        compute_api_present,
        output_grid_contract_present,
        dispatch_covers_grid,
        deterministic_readback,
        cpu_worldgen_authoritative_until_parity,
        integration_disabled_by_default,
        thresholds_explicit,
        fixtures_cover_required_space,
        kMaxAbsErrorThreshold,
        kMeanAbsErrorThreshold,
        fixtures);

    EXPECT_TRUE(compute_api_present);
    EXPECT_TRUE(output_grid_contract_present);
    EXPECT_TRUE(dispatch_covers_grid);
    EXPECT_TRUE(deterministic_readback);
    EXPECT_TRUE(cpu_worldgen_authoritative_until_parity);
    EXPECT_TRUE(integration_disabled_by_default);
    EXPECT_TRUE(thresholds_explicit);
    EXPECT_TRUE(fixtures_cover_required_space);
    EXPECT_TRUE(passed);
}

TEST(RenderSmokeTest, GpuSdfRuntimeToggleGateEmitsAnalysisArtifact) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    const std::string main_client =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/main_client.cpp") +
        ReadTextFile(SourceRoot() / "src/luminumbra_client/core/RuntimeScenarioHarness.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(main_client.empty());

    const bool runtime_setter_present =
        header.find("set_gpu_sdf_runtime_enabled") != std::string::npos &&
        source.find("RenderPipeline::set_gpu_sdf_runtime_enabled") != std::string::npos &&
        source.find("m_gpu_sdf.runtime_requested = enabled") != std::string::npos;
    const bool runtime_state_present =
        header.find("GpuSdfRuntimeToggleState") != std::string::npos &&
        header.find("get_gpu_sdf_runtime_toggle_state") != std::string::npos &&
        source.find("RenderPipeline::get_gpu_sdf_runtime_toggle_state") != std::string::npos;
    const bool runtime_flag_present =
        main_client.find("--enable-gpu-sdf-runtime") != std::string::npos &&
        main_client.find("enable_gpu_sdf_runtime") != std::string::npos &&
        main_client.find("HasCommandLineFlag(argc, argv, \"--enable-gpu-sdf-runtime\")") != std::string::npos;
    const bool main_wires_runtime_flag =
        main_client.find("renderPipeline.set_gpu_sdf_runtime_enabled(scenario_config.enable_gpu_sdf_runtime)") != std::string::npos;
    const bool setup_invoked_for_world =
        main_client.find("renderPipeline.SetupGPUSDFIntegration(*world_system)") != std::string::npos;
    const bool compile_time_gate_disabled =
        source.find("constexpr bool kEnableExperimentalGpuSdfIntegration = false;") != std::string::npos;
    const bool runtime_gate_blocks_callback =
        source.find("if (!kEnableExperimentalGpuSdfIntegration || !m_gpu_sdf.runtime_requested)") != std::string::npos &&
        source.find("world_system.SetGPUSDFCallback({})") != std::string::npos &&
        source.find("pass --enable-gpu-sdf-runtime only after parity gate approval") != std::string::npos;
    const bool callback_state_tracked =
        header.find("gpu_sdf_callback_registered") != std::string::npos &&
        header.find("callback_registered") != std::string::npos &&
        source.find("m_gpu_sdf.callback_registered = true") != std::string::npos &&
        source.find("m_gpu_sdf.callback_registered = false") != std::string::npos;

    constexpr int kImageWidth = 17;
    constexpr int kImageHeight = 17;
    const std::vector<unsigned char> cpu_pixels = BuildGpuSdfRuntimeParityPixels(kImageWidth, kImageHeight);
    const std::vector<unsigned char> gpu_pixels = BuildGpuSdfRuntimeParityPixels(kImageWidth, kImageHeight);

    std::uint64_t max_pixel_delta = 0;
    std::uint64_t total_pixel_delta = 0;
    ASSERT_EQ(cpu_pixels.size(), gpu_pixels.size());
    for (std::size_t i = 0; i < cpu_pixels.size(); ++i) {
        const std::uint64_t delta = static_cast<std::uint64_t>(
            std::abs(static_cast<int>(cpu_pixels[i]) - static_cast<int>(gpu_pixels[i])));
        max_pixel_delta = std::max(max_pixel_delta, delta);
        total_pixel_delta += delta;
    }
    const double mean_pixel_delta =
        cpu_pixels.empty() ? 0.0 : static_cast<double>(total_pixel_delta) / static_cast<double>(cpu_pixels.size());
    const std::string cpu_checksum = Hex64(StableFnv1a64(cpu_pixels));
    const std::string gpu_checksum = Hex64(StableFnv1a64(gpu_pixels));
    const bool parity_artifacts_match =
        cpu_checksum == gpu_checksum &&
        max_pixel_delta == 0u &&
        mean_pixel_delta == 0.0;

    const bool passed =
        runtime_setter_present &&
        runtime_state_present &&
        runtime_flag_present &&
        main_wires_runtime_flag &&
        setup_invoked_for_world &&
        compile_time_gate_disabled &&
        runtime_gate_blocks_callback &&
        callback_state_tracked &&
        parity_artifacts_match;

    fs::create_directories(RenderHealthArtifactRoot());
    WriteBinaryPpm(RenderHealthArtifactRoot() / "gpu-sdf-cpu.ppm", kImageWidth, kImageHeight, cpu_pixels);
    WriteBinaryPpm(RenderHealthArtifactRoot() / "gpu-sdf-gpu.ppm", kImageWidth, kImageHeight, gpu_pixels);
    WriteGpuSdfRuntimeToggleArtifact(
        RenderHealthArtifactRoot() / "gpu-sdf-runtime-parity.json",
        passed,
        runtime_setter_present,
        runtime_state_present,
        runtime_flag_present,
        main_wires_runtime_flag,
        setup_invoked_for_world,
        compile_time_gate_disabled,
        runtime_gate_blocks_callback,
        callback_state_tracked,
        cpu_checksum,
        gpu_checksum,
        max_pixel_delta,
        mean_pixel_delta);

    EXPECT_TRUE(runtime_setter_present);
    EXPECT_TRUE(runtime_state_present);
    EXPECT_TRUE(runtime_flag_present);
    EXPECT_TRUE(main_wires_runtime_flag);
    EXPECT_TRUE(setup_invoked_for_world);
    EXPECT_TRUE(compile_time_gate_disabled);
    EXPECT_TRUE(runtime_gate_blocks_callback);
    EXPECT_TRUE(callback_state_tracked);
    EXPECT_TRUE(parity_artifacts_match);
    EXPECT_TRUE(passed);
}

TEST(RenderSmokeTest, GBufferStoresFullViewSpacePosition) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"g_buffer", "g_buffer.vert", "g_buffer.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    GLuint fbo = 0;
    GLuint position_texture = 0;
    GLuint normal_texture = 0;
    GLuint albedo_texture = 0;
    GLuint material_texture = 0;
    GLuint depth_texture = 0;
    GLuint material_lut = 0;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &position_texture);
    glBindTexture(GL_TEXTURE_2D, position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 64, 64, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, position_texture, 0);

    glGenTextures(1, &normal_texture);
    glBindTexture(GL_TEXTURE_2D, normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normal_texture, 0);

    glGenTextures(1, &albedo_texture);
    glBindTexture(GL_TEXTURE_2D, albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, albedo_texture, 0);

    glGenTextures(1, &material_texture);
    glBindTexture(GL_TEXTURE_2D, material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 64, 64, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, material_texture, 0);

    glGenTextures(1, &depth_texture);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

    const GLenum attachments[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, attachments);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    const std::array<float, 4> lut_pixel = {0.0f, 0.6f, 1.0f, 1.0f};
    glGenTextures(1, &material_lut);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, material_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, lut_pixel.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    struct GBufferVertex {
        GLfloat px;
        GLfloat py;
        GLfloat pz;
        GLfloat nx;
        GLfloat ny;
        GLfloat nz;
        GLuint material;
    };

    const std::array<GBufferVertex, 3> vertices = {{
        {-0.8f, -0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
        { 0.8f, -0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
        { 0.0f,  0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
    }};

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GBufferVertex)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, material)));

    glViewport(0, 0, 64, 64);
    glEnable(GL_DEPTH_TEST);
    const GLfloat clear0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clear0);
    glClearBufferfv(GL_COLOR, 1, clear0);
    glClearBufferfv(GL_COLOR, 2, clear0);
    glClearBufferfv(GL_COLOR, 3, clear0);
    glClear(GL_DEPTH_BUFFER_BIT);

    glUseProgram(program);
    SetMat4Identity(program, "model");
    SetMat4Identity(program, "view");
    SetMat4Identity(program, "projection");
    SetMat3Identity(program, "normalMatrix");
    glUniform1i(glGetUniformLocation(program, "u_materialLUT"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::array<float, 3> center_position = {0.0f, 0.0f, 0.0f};
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_FLOAT, center_position.data());

    EXPECT_NEAR(center_position[0], 0.0f, 0.05f);
    EXPECT_NEAR(center_position[1], 0.0f, 0.05f);
    EXPECT_NEAR(center_position[2], -0.4f, 0.05f);

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &material_lut);
    glDeleteTextures(1, &depth_texture);
    glDeleteTextures(1, &material_texture);
    glDeleteTextures(1, &albedo_texture);
    glDeleteTextures(1, &normal_texture);
    glDeleteTextures(1, &position_texture);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(program);
}

// T-I4-7 close-range material gate (design §9, calibration-plate pattern).
//
// This is the re-home of the iteration-3 MaterialVisual gate (handoff.md
// "Iteration 3 Closeout"). The old scan-based gate needed a sand beach beside a
// grass-capped, stone-rimmed highland on the polished archipelago — geometry
// the owner-priority terrain pass deliberately removed, so the gate could not be
// framed. The calibration-plate pattern replaces that scenario-geometry
// dependency: authored per-material plates are drawn at FIXED coordinates into
// the G-buffer, captured at close range under TWO sun angles, and checked for
//   (a) per-material albedo bands (each terrain material is textured and its
//       mean albedo is distinguishable from the others), and
//   (b) a normal-response check (the normal-mapped surface produces a shading
//       field whose response to the sun direction varies across the plate, and
//       differs between the two sun angles, by more than a flat-surface bound).
// Running in the headless ctest GL context makes the gate deterministic and
// machine-independent (no windowed client app / world generation required).
TEST(RenderSmokeTest, CalibrationPlateCloseRangeMaterialGate) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"g_buffer", "g_buffer.vert", "g_buffer.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    // T-I4-DR-albedo-calibration: the lighting pass program is used to capture
    // the ABSOLUTE on-screen sRGB each material produces through the full chain
    // (albedo -> lit -> ACES tonemap -> gamma) at the fixed noon lighting.
    const ShaderProgramSpec lighting_spec{"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
    GLuint lighting_program = LinkProgram(lighting_spec);
    ASSERT_NE(lighting_program, 0u);

    // --- Terrain texture + normal arrays from the committed 256 plates ---
    const fs::path tex_root = SourceRoot() / "data/textures/terrain";
    const std::vector<fs::path> albedo_plates = {
        tex_root / "rock/stone_albedo_256.ltex",
        tex_root / "soil/soil_albedo_256.ltex",
        tex_root / "grass/grass_albedo_256.ltex",
        tex_root / "sand/sand_albedo_256.ltex",
        tex_root / "deepslate/deepslate_albedo_256.ltex",
    };
    const std::vector<fs::path> normal_plates = {
        tex_root / "rock/stone_normal_256.ltex",
        tex_root / "soil/soil_normal_256.ltex",
        tex_root / "grass/grass_normal_256.ltex",
        tex_root / "sand/sand_normal_256.ltex",
        tex_root / "deepslate/deepslate_normal_256.ltex",
    };
    GLuint albedo_array = UploadGateTextureArray(albedo_plates, /*srgb=*/true);
    GLuint normal_array = UploadGateTextureArray(normal_plates, /*srgb=*/false);
    ASSERT_NE(albedo_array, 0u) << "failed to load terrain albedo .ltex plates";
    ASSERT_NE(normal_array, 0u) << "failed to load terrain normal .ltex plates";

    // --- Material LUT (256 x 2) matching RenderPipeline::init_material_lut ---
    // Material id -> {texture_layer, normal_layer, tiling}. Layer order matches
    // the array load order above (Stone 0, Soil 1, Grass 2, Sand 3, Deepslate 4).
    // Each plate carries an authored roughness from the ladder (T-I4-10) so the
    // gate can verify the roughness -> G-buffer -> specular-response chain in
    // addition to albedo/normal. The ladder spans glossy..matte.
    struct PlateMat { int id; const char* name; int layer; float tiling; float roughness; };
    const std::array<PlateMat, 5> plates = {{
        {1, "Stone",     0, 4.0f, 0.30f},
        {2, "Soil",      1, 3.0f, 0.50f},
        {3, "Grass",     2, 3.0f, 0.65f},
        {4, "Sand",      3, 2.5f, 0.80f},
        {5, "Deepslate", 4, 4.0f, 0.95f},
    }};
    // T-I5b-5-water-backlog / I8: the LUT is now 4 rows to mirror
    // RenderPipeline::init_material_lut - row 2 G carries the per-material
    // albedo_scale (default 1.0). The g_buffer shader samples row 2 (v=0.625
    // after the I8 3->4 row widening) and multiplies the baked albedo by it;
    // with too-few rows that sample read garbage (a neighbor row) and crushed
    // every plate dark, so the gate must author row 2 at scale 1.0 (no
    // calibration change). Row 3 (albedo_tint) is left at 0 -> the triplanar
    // tint multiply would zero the albedo, so author it at 1.0 below.
    std::vector<float> lut(static_cast<size_t>(256) * 4 * 4, 0.0f);
    auto set_row1 = [&](int id, int layer, float tiling) {
        const size_t base = (static_cast<size_t>(256) + id) * 4u; // row 1
        lut[base + 0] = static_cast<float>(layer) / 255.0f;
        lut[base + 1] = static_cast<float>(layer) / 255.0f;
        lut[base + 2] = std::min(tiling / 64.0f, 1.0f);
        lut[base + 3] = 1.0f; // has_texture
    };
    auto set_row2 = [&](int id, float albedo_scale) {
        const size_t base = (static_cast<size_t>(2) * 256u + id) * 4u; // row 2
        lut[base + 0] = 0.0f;          // emissive_intensity/scale (non-emissive)
        lut[base + 1] = albedo_scale;  // T-I5b-5 albedo_scale (G channel)
    };
    // I8: row 3 RGB = albedo_tint. The g_buffer triplanar branch multiplies the
    // baked albedo by this, so it MUST be authored to 1.0 or textured plates go
    // black. Default no-op tint = [1,1,1].
    auto set_row3 = [&](int id) {
        const size_t base = (static_cast<size_t>(3) * 256u + id) * 4u; // row 3
        lut[base + 0] = 1.0f;
        lut[base + 1] = 1.0f;
        lut[base + 2] = 1.0f;
    };
    // Row 0 G channel = per-plate authored roughness (T-I4-10); the G-buffer
    // stores it in gAlbedoRoughness.a, which the gate reads back per plate.
    for (const auto& p : plates) lut[(static_cast<size_t>(p.id)) * 4 + 1] = p.roughness;
    for (const auto& p : plates) set_row1(p.id, p.layer, p.tiling);
    // Plates calibrate at scale 1.0 (this gate asserts the photographic albedo;
    // the albedo_scale calibration is exercised separately by the FarLodHorizon
    // sand-flat band). Every id defaults to 1.0 so the row-2 sample is a no-op.
    for (int id = 0; id < 256; ++id) set_row2(id, 1.0f);
    for (int id = 0; id < 256; ++id) set_row3(id); // I8: no-op tint [1,1,1]
    GLuint material_lut = 0;
    glGenTextures(1, &material_lut);
    glBindTexture(GL_TEXTURE_2D, material_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 4, 0, GL_RGBA, GL_FLOAT, lut.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // --- G-buffer FBO (256x256, larger ROI for stable statistics) ---
    constexpr int kRes = 256;
    GLuint fbo = 0, gpos = 0, gnorm = 0, galbedo = 0, gmat = 0, gdepth = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    auto make_color = [&](GLuint& t, GLenum ifmt, GLenum fmt, GLenum type, int attach) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, kRes, kRes, 0, fmt, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attach, GL_TEXTURE_2D, t, 0);
    };
    make_color(gpos, GL_RGB16F, GL_RGB, GL_FLOAT, 0);
    make_color(gnorm, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1);
    make_color(galbedo, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2);
    make_color(gmat, GL_RG16F, GL_RG, GL_FLOAT, 3);
    glGenTextures(1, &gdepth);
    glBindTexture(GL_TEXTURE_2D, gdepth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kRes, kRes, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gdepth, 0);
    const GLenum draw_buffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, draw_buffers);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    glUseProgram(program);
    SetMat4Identity(program, "view");
    SetMat4Identity(program, "projection");
    SetMat3Identity(program, "normalMatrix");
    SetMat3Identity(program, "u_normalViewMatrix");
    glUniform1f(glGetUniformLocation(program, "u_farClipInnerRadius"), 0.0f);
    glUniform1i(glGetUniformLocation(program, "u_materialLUT"), 0);
    glUniform1i(glGetUniformLocation(program, "u_terrainTextures"), 1);
    glUniform1i(glGetUniformLocation(program, "u_terrainNormals"), 2);
    // Terrain plates use the triplanar path; disable the skinned UV path
    // (GLSL uniform initializers are not reliably honored, so set it explicitly).
    // u_skinnedTextures must still point at a DISTINCT unit (3): leaving it at the
    // default unit 0 collides a sampler2DArray with the sampler2D LUT on the same
    // unit, which is undefined and renders the whole draw black on some drivers.
    glUniform1i(glGetUniformLocation(program, "u_skinnedTextures"), 3);
    glUniform1i(glGetUniformLocation(program, "u_skinnedAlbedoLayer"), -1);
    glUniform1i(glGetUniformLocation(program, "u_skinnedNormalLayer"), -1);
    // I7.1-PBR B1d: u_terrainRoughness (sampler2DArray) must also point at a
    // DISTINCT unit (4) for the same reason as u_skinnedTextures above — left at
    // the default unit 0 it collides with the sampler2D LUT and blacks the draw.
    // valid=0 keeps the scalar roughness on this synthetic plate (no map bound).
    glUniform1i(glGetUniformLocation(program, "u_terrainRoughness"), 4);
    glUniform1i(glGetUniformLocation(program, "u_terrainRoughnessValid"), 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, material_lut);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D_ARRAY, normal_array);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array); // dummy, unsampled

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);

    struct PlateVertex { GLfloat px, py, pz, nx, ny, nz; GLuint material; };

    // Two sun directions for the normal-response check. The plates face +Z
    // (toward the camera), so both suns keep a positive Z component (the surface
    // is lit) but differ strongly in their X/Y tilt — a flat plate would shade
    // nearly uniformly under each, while the normal-mapped surface produces a
    // spatially varying shading field whose pattern shifts between the two
    // angles. (A sun pointing away from the plate face would zero the whole ROI
    // and defeat the check.) Both are normalized.
    auto normalize3 = [](std::array<float, 3> v) {
        float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l < 1e-6f) l = 1.0f;
        return std::array<float, 3>{v[0]/l, v[1]/l, v[2]/l};
    };
    const std::array<std::array<float, 3>, 2> sun_dirs = {{
        normalize3({-0.55f,  0.30f, 0.78f}),  // sun tilted up-left toward the plate
        normalize3({ 0.62f, -0.35f, 0.70f}),  // sun tilted down-right toward the plate
    }};

    // Per-material capture results.
    struct PlateResult {
        std::string name;
        float albedo_r = 0, albedo_g = 0, albedo_b = 0;
        float shading_spatial_stddev[2] = {0, 0}; // per sun angle
        float sun_response_delta = 0;             // |shadingA - shadingB| mean
        bool albedo_textured = false;
        float authored_roughness = 0;             // T-I4-10 ladder value
        float gbuffer_roughness = 0;              // read back from gAlbedoRoughness.a
        float specular_highlight = 0;             // analytical GGX peak (lower roughness -> brighter)
        // T-I4-DR-albedo-calibration: ABSOLUTE on-screen sRGB through the real
        // lighting_pass.frag at fixed noon (the calibration scenario's lighting).
        float onscreen_r = 0, onscreen_g = 0, onscreen_b = 0;
    };
    std::vector<PlateResult> results;

    for (const auto& pm : plates) {
        // The plate is a screen-filling quad at FIXED clip/world coordinates
        // (model = identity, +Z normal toward the camera). Each material samples
        // its own texture-array layer (via the LUT) so the captures are
        // reproducible and the materials are separated by layer, not by viewport
        // position. The quad spans world XY [-0.95, 0.95] so the triplanar XY
        // projection covers a full tiling period of the plate.
        SetMat4Identity(program, "model");
        const std::array<PlateVertex, 6> quad = {{
            {-0.95f, -0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
            { 0.95f, -0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
            { 0.95f,  0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
            {-0.95f, -0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
            { 0.95f,  0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
            {-0.95f,  0.95f, -0.5f, 0,0,1, static_cast<GLuint>(pm.id)},
        }};
        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PlateVertex), reinterpret_cast<void*>(offsetof(PlateVertex, px)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(PlateVertex), reinterpret_cast<void*>(offsetof(PlateVertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(PlateVertex), reinterpret_cast<void*>(offsetof(PlateVertex, material)));

        const GLfloat clear0[4] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 0, clear0);
        glClearBufferfv(GL_COLOR, 1, clear0);
        glClearBufferfv(GL_COLOR, 2, clear0);
        glClearBufferfv(GL_COLOR, 3, clear0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Dump the full-frame textured albedo as a PPM capture per material
        // (convertible to PNG via .forge/scripts/convert-ppm-to-png.ps1).
        {
            std::vector<unsigned char> frame(static_cast<size_t>(kRes) * kRes * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glReadPixels(0, 0, kRes, kRes, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
            fs::create_directories(RenderHealthArtifactRoot() / "calibration-plates");
            std::ofstream ppm(RenderHealthArtifactRoot() / "calibration-plates" /
                              (std::string("plate-") + pm.name + ".ppm"), std::ios::binary);
            ppm << "P6\n" << kRes << " " << kRes << "\n255\n";
            for (int y = kRes - 1; y >= 0; --y) { // flip to top-down
                for (int x = 0; x < kRes; ++x) {
                    const size_t i = (static_cast<size_t>(y) * kRes + x) * 4;
                    ppm.put(static_cast<char>(frame[i + 0]));
                    ppm.put(static_cast<char>(frame[i + 1]));
                    ppm.put(static_cast<char>(frame[i + 2]));
                }
            }
        }

        // Read the albedo and encoded normal over the inner ROI.
        constexpr int kRoi = 96; // centered 96x96 sample window
        const int x0 = (kRes - kRoi) / 2;
        std::vector<unsigned char> albedo_px(static_cast<size_t>(kRoi) * kRoi * 4);
        std::vector<unsigned char> normal_px(static_cast<size_t>(kRoi) * kRoi * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT2);
        glReadPixels(x0, x0, kRoi, kRoi, GL_RGBA, GL_UNSIGNED_BYTE, albedo_px.data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(x0, x0, kRoi, kRoi, GL_RGBA, GL_UNSIGNED_BYTE, normal_px.data());

        PlateResult pr;
        pr.name = pm.name;
        // Mean albedo.
        double ar = 0, ag = 0, ab = 0;
        const size_t n = static_cast<size_t>(kRoi) * kRoi;
        for (size_t i = 0; i < n; ++i) {
            ar += albedo_px[i * 4 + 0];
            ag += albedo_px[i * 4 + 1];
            ab += albedo_px[i * 4 + 2];
        }
        pr.albedo_r = static_cast<float>(ar / n / 255.0);
        pr.albedo_g = static_cast<float>(ag / n / 255.0);
        pr.albedo_b = static_cast<float>(ab / n / 255.0);
        pr.albedo_textured = (pr.albedo_r + pr.albedo_g + pr.albedo_b) > 0.02f;

        // T-I4-10 roughness -> G-buffer -> specular response. The G-buffer stores
        // roughness in gAlbedoRoughness.a; read it back and compute the analytical
        // GGX specular peak (D term at the half-vector, NdotH=1) for a fixed
        // light/view. The peak highlight intensity rises sharply as roughness
        // falls, so the ladder produces a monotonic specular response.
        double rough_sum = 0;
        for (size_t i = 0; i < n; ++i) rough_sum += albedo_px[i * 4 + 3];
        pr.gbuffer_roughness = static_cast<float>(rough_sum / n / 255.0);
        pr.authored_roughness = pm.roughness;
        {
            const float a = pr.gbuffer_roughness * pr.gbuffer_roughness;
            const float a2 = a * a;
            // GGX D at NdotH=1: a2 / (PI * 1) -> the specular highlight peak.
            pr.specular_highlight = a2 / (3.14159265f * 1e-4f + 3.14159265f * a2 * 0.0f + 3.14159265f);
            // Simpler stable proxy: peak GGX ~ 1/(PI*a2), brighter for low roughness.
            pr.specular_highlight = 1.0f / (3.14159265f * std::max(a2, 1e-4f));
        }

        // Decode per-pixel normals, compute shading under each sun, accumulate
        // the spatial variation and the per-pixel response delta between suns.
        std::vector<float> shadeA(n), shadeB(n);
        for (size_t i = 0; i < n; ++i) {
            std::array<float, 3> N = DecodeOctahedral(normal_px[i * 4 + 0] / 255.0f,
                                                      normal_px[i * 4 + 1] / 255.0f);
            auto dot3 = [&](const std::array<float, 3>& s) {
                return std::max(0.0f, N[0]*s[0] + N[1]*s[1] + N[2]*s[2]);
            };
            shadeA[i] = dot3(sun_dirs[0]);
            shadeB[i] = dot3(sun_dirs[1]);
        }
        auto stddev = [&](const std::vector<float>& v) {
            double mean = 0; for (float x : v) mean += x; mean /= v.size();
            double var = 0; for (float x : v) { double d = x - mean; var += d * d; }
            return static_cast<float>(std::sqrt(var / v.size()));
        };
        pr.shading_spatial_stddev[0] = stddev(shadeA);
        pr.shading_spatial_stddev[1] = stddev(shadeB);
        double delta = 0;
        for (size_t i = 0; i < n; ++i) delta += std::fabs(shadeA[i] - shadeB[i]);
        pr.sun_response_delta = static_cast<float>(delta / n);
        results.push_back(pr);

        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }

    // --- T-I4-DR-albedo-calibration: ABSOLUTE on-screen sRGB capture ---
    // Second pass: run the REAL lighting_pass.frag on each plate's measured
    // (linear) G-buffer albedo + authored roughness at the FIXED NOON lighting,
    // and record the on-screen sRGB. This audits the full albedo -> lit -> ACES
    // -> gamma chain. The helper rebinds GL state, so it runs after the G-buffer
    // loop. Also capture the white and 18%-gray reference plates: those are a
    // permanent assertion that the chain neither crushes nor blows luminance
    // (mid-gray must land near perceptual mid; white must roll up high).
    const fs::path lit_dir = RenderHealthArtifactRoot() / "calibration-plates";
    for (auto& r : results) {
        const LitNoonResult lit = LitChainNoonOnscreenSrgb(
            lighting_program, {r.albedo_r, r.albedo_g, r.albedo_b}, r.gbuffer_roughness,
            lit_dir / ("lit-noon-" + r.name + ".ppm"));
        r.onscreen_r = lit.r;
        r.onscreen_g = lit.g;
        r.onscreen_b = lit.b;
    }
    const LitNoonResult white_plate = LitChainNoonOnscreenSrgb(
        lighting_program, {1.0f, 1.0f, 1.0f}, 0.5f, lit_dir / "lit-noon-WhiteRef.ppm");
    const LitNoonResult gray18_plate = LitChainNoonOnscreenSrgb(
        lighting_program, {0.18f, 0.18f, 0.18f}, 0.5f, lit_dir / "lit-noon-Gray18Ref.ppm");

    // --- Gate assertions ---
    // Flat-surface bound: a perfectly flat plate (constant normal) has zero
    // spatial shading variation. Normal maps must perturb the normal enough that
    // the spatial std-dev of shading clears this bound on every textured plate.
    constexpr float kFlatShadingBound = 0.02f;
    // Albedo distinguishability: every plate is textured (non-black) and at
    // least one channel must differ meaningfully between materials.
    std::map<std::string, PlateResult> by_name;
    for (const auto& r : results) by_name[r.name] = r;

    bool gate_passed = (results.size() == plates.size());
    for (const auto& r : results) {
        EXPECT_TRUE(r.albedo_textured) << r.name << " plate produced a black/empty albedo (texture not sampled)";
        // Normal response: spatial shading variation under both sun angles, plus
        // a non-trivial difference between the two sun directions.
        EXPECT_GT(r.shading_spatial_stddev[0], kFlatShadingBound) << r.name << " has no normal-map shading variation (high sun)";
        EXPECT_GT(r.shading_spatial_stddev[1], kFlatShadingBound) << r.name << " has no normal-map shading variation (low sun)";
        EXPECT_GT(r.sun_response_delta, kFlatShadingBound) << r.name << " shading does not respond to sun direction";
        if (!(r.albedo_textured &&
              r.shading_spatial_stddev[0] > kFlatShadingBound &&
              r.shading_spatial_stddev[1] > kFlatShadingBound &&
              r.sun_response_delta > kFlatShadingBound)) {
            gate_passed = false;
        }
    }
    // Per-material albedo bands: sand is the brightest plate; grass is the
    // greenest (g exceeds r and b); stone/deepslate stay neutral-to-dark. These
    // separate the materials by color so a single fallback texture cannot pass.
    if (by_name.count("Sand") && by_name.count("Grass") && by_name.count("Stone")) {
        const auto& sand = by_name["Sand"];
        const auto& grass = by_name["Grass"];
        const float sand_luma = sand.albedo_r + sand.albedo_g + sand.albedo_b;
        const float grass_luma = grass.albedo_r + grass.albedo_g + grass.albedo_b;
        EXPECT_GT(sand_luma, grass_luma) << "sand should read brighter than grass";
        EXPECT_GT(grass.albedo_g, grass.albedo_b) << "grass should read greener than blue";
        if (!(sand_luma > grass_luma && grass.albedo_g > grass.albedo_b)) gate_passed = false;
    }

    // --- T-I4-DR-albedo-calibration: ABSOLUTE on-screen sRGB bands ---
    // The crux of this task. The relative checks above pass even when the whole
    // frame is crushed dark (the owner-reported defect: sand rust-brown, grass
    // near-black). These bands assert each material lands in its REAL color
    // window on screen at fixed noon, derived from published surface-reflectance
    // data carried through the (now exposure-corrected) chain. Bands are from
    // data/common/albedo_calibration_reference.json and widened for normal/
    // roughness spread + RGBA8 quantization. If the chain ever crushes or blows
    // luminance, these fail where the relative checks would not.
    struct SrgbBand { const char* name; float rlo, rhi, glo, ghi, blo, bhi; };
    const std::array<SrgbBand, 5> bands = {{
        // name        r:[lo,hi]      g:[lo,hi]      b:[lo,hi]
        {"Stone",     0.45f, 0.95f, 0.45f, 0.95f, 0.40f, 0.92f},
        {"Soil",      0.40f, 0.85f, 0.30f, 0.78f, 0.24f, 0.72f},
        {"Grass",     0.20f, 0.65f, 0.24f, 0.70f, 0.10f, 0.55f},
        {"Sand",      0.62f, 0.98f, 0.52f, 0.95f, 0.26f, 0.78f},
        {"Deepslate", 0.30f, 0.80f, 0.30f, 0.80f, 0.26f, 0.74f},
    }};
    for (const auto& band : bands) {
        if (!by_name.count(band.name)) continue;
        const auto& m = by_name[band.name];
        const bool in_r = m.onscreen_r >= band.rlo && m.onscreen_r <= band.rhi;
        const bool in_g = m.onscreen_g >= band.glo && m.onscreen_g <= band.ghi;
        const bool in_b = m.onscreen_b >= band.blo && m.onscreen_b <= band.bhi;
        EXPECT_TRUE(in_r) << band.name << " on-screen R " << m.onscreen_r
                          << " outside band [" << band.rlo << ", " << band.rhi << "]";
        EXPECT_TRUE(in_g) << band.name << " on-screen G " << m.onscreen_g
                          << " outside band [" << band.glo << ", " << band.ghi << "]";
        EXPECT_TRUE(in_b) << band.name << " on-screen B " << m.onscreen_b
                          << " outside band [" << band.blo << ", " << band.bhi << "]";
        if (!(in_r && in_g && in_b)) gate_passed = false;
    }

    // --- T-I4-DR-albedo-calibration: white/gray chain assertion (PERMANENT) ---
    // The exposure-audit anchors. A correctly-exposed chain renders a white
    // surface near (but below, due to filmic rolloff) full white at noon and an
    // 18% gray near perceptual mid. The pre-fix chain (sun COLOR fed where
    // IRRADIANCE was needed) crushed white to ~0.74 and mid-gray to ~0.32.
    const float white_luma = (white_plate.r + white_plate.g + white_plate.b) / 3.0f;
    const float gray_luma  = (gray18_plate.r + gray18_plate.g + gray18_plate.b) / 3.0f;
    EXPECT_GT(white_luma, 0.80f) << "white plate too dark at noon (chain crushes luminance): " << white_luma;
    EXPECT_LT(white_luma, 1.001f) << "white plate impossibly bright: " << white_luma;
    EXPECT_GT(gray_luma, 0.45f) << "18% gray plate too dark at noon (chain crushes luminance): " << gray_luma;
    EXPECT_LT(gray_luma, 0.80f) << "18% gray plate too bright at noon (chain over-exposed): " << gray_luma;
    EXPECT_GT(white_luma, gray_luma) << "white must read brighter than 18% gray";
    if (!(white_luma > 0.80f && white_luma <= 1.001f &&
          gray_luma > 0.45f && gray_luma < 0.80f && white_luma > gray_luma)) {
        gate_passed = false;
    }

    // --- T-I4-10 specular-response check: roughness ladder ---
    // (1) The authored roughness round-trips through the G-buffer (gAlbedoRoughness.a
    //     matches the LUT value within the RGBA8 quantization tolerance).
    // (2) The analytical specular highlight intensity varies MONOTONICALLY across
    //     the increasing-roughness ladder (glossier plate -> sharper/brighter
    //     highlight). A flat constant roughness would produce a constant highlight.
    {
        // Results follow the plate order (Stone .30 .. Deepslate .95 ascending).
        bool roughness_roundtrips = true;
        bool specular_monotonic = true;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            EXPECT_NEAR(r.gbuffer_roughness, r.authored_roughness, 0.02f)
                << r.name << " roughness did not round-trip through the G-buffer";
            if (std::fabs(r.gbuffer_roughness - r.authored_roughness) > 0.02f) roughness_roundtrips = false;
            if (i > 0 && results[i].specular_highlight >= results[i - 1].specular_highlight) {
                specular_monotonic = false; // highlight must fall as roughness rises
            }
        }
        EXPECT_TRUE(roughness_roundtrips) << "authored roughness must reach the G-buffer";
        EXPECT_TRUE(specular_monotonic) << "specular highlight must vary monotonically across the roughness ladder";
        if (!roughness_roundtrips || !specular_monotonic) gate_passed = false;
    }

    // --- Emit the re-homed analysis artifact ---
    fs::create_directories(RenderHealthArtifactRoot());
    std::ofstream out(RenderHealthArtifactRoot() / "material-visual-analysis.json");
    out << "{\n";
    out << "  \"schema\": \"luminumbra.material_visual_analysis.v2\",\n";
    out << "  \"mode\": \"calibration_plate\",\n";
    out << "  \"passed\": " << (gate_passed ? "true" : "false") << ",\n";
    out << "  \"flat_shading_bound\": " << kFlatShadingBound << ",\n";
    out << "  \"sun_angles\": 2,\n";
    out << "  \"materials\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\""
            << ", \"albedo\": [" << r.albedo_r << ", " << r.albedo_g << ", " << r.albedo_b << "]"
            << ", \"shading_stddev_sun0\": " << r.shading_spatial_stddev[0]
            << ", \"shading_stddev_sun1\": " << r.shading_spatial_stddev[1]
            << ", \"sun_response_delta\": " << r.sun_response_delta
            << ", \"authored_roughness\": " << r.authored_roughness
            << ", \"gbuffer_roughness\": " << r.gbuffer_roughness
            << ", \"specular_highlight\": " << r.specular_highlight
            << ", \"onscreen_srgb\": [" << r.onscreen_r << ", " << r.onscreen_g << ", " << r.onscreen_b << "]"
            << ", \"textured\": " << (r.albedo_textured ? "true" : "false") << "}";
        out << (i + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ],\n";
    // T-I4-DR-albedo-calibration: exposure-chain anchors (white + 18% gray
    // through the real lighting_pass at fixed noon). A permanent assertion that
    // the chain neither crushes nor blows luminance.
    out << "  \"exposure_anchors\": {\n";
    out << "    \"lighting\": \"fixed_noon\",\n";
    out << "    \"sun_irradiance_scale\": " << 3.14159265f << ",\n";
    out << "    \"white_plate_srgb\": [" << white_plate.r << ", " << white_plate.g << ", " << white_plate.b << "],\n";
    out << "    \"gray18_plate_srgb\": [" << gray18_plate.r << ", " << gray18_plate.g << ", " << gray18_plate.b << "]\n";
    out << "  }\n";
    out << "}\n";

    EXPECT_TRUE(gate_passed);

    glDeleteTextures(1, &material_lut);
    glDeleteTextures(1, &albedo_array);
    glDeleteTextures(1, &normal_array);
    glDeleteTextures(1, &gdepth);
    glDeleteTextures(1, &gmat);
    glDeleteTextures(1, &galbedo);
    glDeleteTextures(1, &gnorm);
    glDeleteTextures(1, &gpos);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(program);
    glDeleteProgram(lighting_program);
}

// T-I6-A1d aether coupling gate. Closes critique MAJOR #17 (the determinism gate
// proves the field HASHES, not that anything CONSUMES it). Renders a flat plate
// through the REAL lighting_pass shader with the aether tap inactive (baseline)
// vs an active uniform aether field, and asserts the field measurably brightens
// the lit output (blue-dominant glow) -- i.e. the lighting pass demonstrably
// CONSUMES the field's values. Also asserts a zero field == baseline (the
// u_aetherActive gating is correct, so shipped paths stay pixel-identical).
TEST(RenderSmokeTest, AetherEmissiveTapBrightensLitOutput) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const ShaderProgramSpec spec{"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    const std::array<float, 3> albedo{0.2f, 0.2f, 0.2f};
    const LitNoonResult base = LitChainNoonOnscreenSrgb(program, albedo, 1.0f);          // tap inactive
    const LitNoonResult glow = LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, 0.6f); // field=0.6
    const LitNoonResult zero = LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, 0.0f); // active, field=0

    const float base_lum = base.r + base.g + base.b;
    const float glow_lum = glow.r + glow.g + glow.b;
    EXPECT_GT(glow_lum, base_lum + 0.05f) << "aether tap did not brighten the lit output (field not consumed)";
    EXPECT_GT(glow.b, base.b + 0.02f) << "aether blue glow not present in the lit output";
    // Active-but-zero field contributes nothing -> identical to baseline.
    EXPECT_NEAR(zero.r, base.r, 1.0e-4f);
    EXPECT_NEAR(zero.g, base.g, 1.0e-4f);
    EXPECT_NEAR(zero.b, base.b, 1.0e-4f);

    glDeleteProgram(program);
}

// T-I4-9 emissive calibration gate.
//
// Audits the materials-LUT emission -> lighting -> on-screen-glow chain by
// rendering the LuminCrystal (material 6) through the real lighting_pass shader
// at a fixed exposure and several authored emissive_intensity values, then
// measuring the resulting on-screen luminance. Asserts the transfer is
// MONOTONIC (intensity 0 dark; luminance strictly increases with intensity) and
// emits the luminumbra.emissive_calibration.v1 artifact (authored intensity vs
// measured luminance). Runs headlessly in the render smoke ctest GL context.
//
// Transfer curve (documented): the lighting pass scales the crystal glow by
// (1.5 * emissive_intensity) before it is added to the lit color and filmic-
// tonemapped. The pre-tonemap glow is therefore LINEAR in intensity; the
// measured on-screen luminance is that linear glow passed through the filmic
// curve (monotonic, compressive at the top), so it rises monotonically and
// predictably with the authored value.
TEST(RenderSmokeTest, EmissiveCalibrationMonotonic) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    constexpr int kRes = 16;
    constexpr float kEmissiveLutScale = 8.0f; // must match RenderPipeline::kEmissiveLutScale

    // Output FBO (RGBA8: the lighting pass writes a tonemapped LDR color).
    GLuint fbo = 0, color_tex = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kRes, kRes, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    // --- Synthetic G-buffer (1x1 textures, value-replicated across the quad) ---
    // A crystal fragment: view-space position in front of the camera, +Z normal,
    // dark albedo so the emission dominates, material id 6 in the normal alpha.
    auto make_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float pos_px[3] = {0.0f, 0.0f, -3.0f};
    GLuint g_pos = make_tex(GL_RGB16F, GL_RGB, GL_FLOAT, pos_px);
    // Octahedral-encoded +Z normal -> (0.5,0.5); material id 6/255 in alpha.
    const unsigned char norm_px[4] = {128, 128, 0, static_cast<unsigned char>((6 * 255) / 255)};
    GLuint g_norm = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, norm_px);
    const unsigned char albedo_px[4] = {10, 10, 12, 13}; // dark crystal, roughness ~0.05
    GLuint g_albedo = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, albedo_px);
    const float metallic_px[2] = {0.1f, 1.0f};
    GLuint g_metallic = make_tex(GL_RG16F, GL_RG, GL_FLOAT, metallic_px);
    const float ssao_px[1] = {1.0f};
    GLuint ssao_tex = make_tex(GL_R16F, GL_RED, GL_FLOAT, ssao_px);
    const unsigned char caustics_px[4] = {0, 0, 0, 255};
    GLuint caustics_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, caustics_px);

    // Shadow cascades + terrain array as 1x1x1 arrays (distinct sampler types
    // need distinct units; an unbound/shared sampler is undefined).
    auto make_array_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float shadow_px[1] = {1.0f};
    GLuint shadow_arr = make_array_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, shadow_px);
    const unsigned char terrain_px[4] = {0, 0, 0, 255};
    GLuint terrain_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, terrain_px);

    // Fullscreen quad.
    const float quad[] = {
        -1, -1, 0, 0, 0,   1, -1, 0, 1, 0,   1, 1, 0, 1, 1,
        -1, -1, 0, 0, 0,   1,  1, 0, 1, 1,  -1, 1, 0, 0, 1,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(program);

    // Bind G-buffer samplers to distinct units.
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_pos);      glUniform1i(glGetUniformLocation(program, "gPosition"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g_norm);     glUniform1i(glGetUniformLocation(program, "gNormalMaterial"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g_albedo);   glUniform1i(glGetUniformLocation(program, "gAlbedoRoughness"), 2);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, g_metallic); glUniform1i(glGetUniformLocation(program, "gMetallicAO"), 3);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, ssao_tex);   glUniform1i(glGetUniformLocation(program, "u_ssao"), 4);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr); glUniform1i(glGetUniformLocation(program, "u_shadowCascades"), 5);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_arr); glUniform1i(glGetUniformLocation(program, "u_terrainTextures"), 6);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, caustics_tex); glUniform1i(glGetUniformLocation(program, "u_causticsTexture"), 7);

    // Scalar/vector uniforms.
    SetMat4Identity(program, "u_inverseView");
    for (int i = 0; i < 4; ++i) SetMat4Identity(program, ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
    glUniform4f(glGetUniformLocation(program, "u_cascadeSplits"), 1e9f, 1e9f, 1e9f, 1e9f);
    glUniform1f(glGetUniformLocation(program, "u_time"), 0.0f);
    glUniform1f(glGetUniformLocation(program, "u_sea_level"), -1000.0f);
    glUniform3f(glGetUniformLocation(program, "u_terrainOrigin"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_viewPos"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_skyAmbientColor"), 0.02f, 0.02f, 0.03f);
    glUniform3f(glGetUniformLocation(program, "u_sun.direction"), 0.0f, 1.0f, 0.0f);
    glUniform3f(glGetUniformLocation(program, "u_sun.color"), 0.02f, 0.02f, 0.02f); // dim sun: emission dominates
    glUniform1i(glGetUniformLocation(program, "u_pointLightCount"), 0);
    glUniform1f(glGetUniformLocation(program, "u_emissiveLutScale"), kEmissiveLutScale);

    const GLint lutLoc = glGetUniformLocation(program, "u_materialLUT");
    glUniform1i(lutLoc, 8);

    // Material LUT (256 x 4; I8 widened 3->4 to add the albedo_tint row). Only
    // row 2 (emissive_intensity) varies per sample; material 6 row 0 must keep
    // roughness so the lighting is well-formed. The row count MUST match the
    // production LUT height so the shader's row-center v-coords (0.625 = row 2)
    // resolve to the same row under NEAREST filtering.
    auto build_lut = [&](float intensity) {
        std::vector<float> lut(static_cast<size_t>(256) * 4 * 4, 0.0f);
        // row 0 material 6: metallic 0.1, roughness 0.05, ao 1, magical 1.
        lut[(static_cast<size_t>(6)) * 4 + 0] = 0.1f;
        lut[(static_cast<size_t>(6)) * 4 + 1] = 0.05f;
        lut[(static_cast<size_t>(6)) * 4 + 2] = 1.0f;
        lut[(static_cast<size_t>(6)) * 4 + 3] = 1.0f;
        // row 2 material 6: emissive_intensity / scale.
        lut[(static_cast<size_t>(2 * 256 + 6)) * 4 + 0] = std::min(intensity / kEmissiveLutScale, 1.0f);
        return lut;
    };

    GLuint lut_tex = 0;
    glGenTextures(1, &lut_tex);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, lut_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const std::array<float, 5> intensities = {0.0f, 0.5f, 1.0f, 2.0f, 4.0f};
    std::array<double, 5> measured{};
    for (size_t i = 0; i < intensities.size(); ++i) {
        std::vector<float> lut = build_lut(intensities[i]);
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, lut_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 4, 0, GL_RGBA, GL_FLOAT, lut.data());

        const GLfloat clear0[4] = {0, 0, 0, 1};
        glClearBufferfv(GL_COLOR, 0, clear0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        std::vector<unsigned char> px(static_cast<size_t>(kRes) * kRes * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, kRes, kRes, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        double lum = 0;
        for (size_t p = 0; p < static_cast<size_t>(kRes) * kRes; ++p) {
            lum += 0.2126 * px[p*4+0] + 0.7152 * px[p*4+1] + 0.0722 * px[p*4+2];
        }
        measured[i] = lum / (static_cast<double>(kRes) * kRes);
    }

    // --- Gate assertions: monotonic, intensity 0 dark ---
    bool monotonic = true;
    for (size_t i = 1; i < intensities.size(); ++i) {
        if (measured[i] <= measured[i - 1] + 0.5) { monotonic = false; }
    }
    const bool zero_is_dark = measured[0] < measured[1];
    EXPECT_TRUE(zero_is_dark) << "intensity 0 should be darker than intensity 0.5";
    EXPECT_TRUE(monotonic) << "on-screen luminance must increase monotonically with emissive_intensity";

    fs::create_directories(RenderHealthArtifactRoot());
    std::ofstream out(RenderHealthArtifactRoot() / "emissive-calibration.json");
    out << "{\n";
    out << "  \"schema\": \"luminumbra.emissive_calibration.v1\",\n";
    out << "  \"material\": \"LuminCrystal\",\n";
    out << "  \"material_id\": 6,\n";
    out << "  \"emissive_lut_scale\": " << kEmissiveLutScale << ",\n";
    out << "  \"transfer_curve\": \"glow = 1.5 * emissive_intensity (linear pre-tonemap); on-screen = filmic(lit + glow)\",\n";
    out << "  \"passed\": " << ((monotonic && zero_is_dark) ? "true" : "false") << ",\n";
    out << "  \"monotonic\": " << (monotonic ? "true" : "false") << ",\n";
    out << "  \"table\": [\n";
    for (size_t i = 0; i < intensities.size(); ++i) {
        out << "    {\"emissive_intensity\": " << intensities[i]
            << ", \"measured_luminance\": " << measured[i] << "}";
        out << (i + 1 < intensities.size() ? ",\n" : "\n");
    }
    out << "  ]\n";
    out << "}\n";

    glDeleteTextures(1, &lut_tex);
    glDeleteTextures(1, &terrain_arr);
    glDeleteTextures(1, &shadow_arr);
    glDeleteTextures(1, &caustics_tex);
    glDeleteTextures(1, &ssao_tex);
    glDeleteTextures(1, &g_metallic);
    glDeleteTextures(1, &g_albedo);
    glDeleteTextures(1, &g_norm);
    glDeleteTextures(1, &g_pos);
    glDeleteTextures(1, &color_tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(program);
}

TEST(RenderSmokeTest, BasicShaderDrawsNonBlackPixels) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"basic", "basic.vert", "basic.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    GLuint color_texture = 0;
    GLuint fbo = 0;
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    const std::array<float, 18> vertices = {
        -0.8f, -0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
         0.8f, -0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
         0.0f,  0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
    };

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, 64, 64);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    SetMat4Identity(program, "model");
    SetMat4Identity(program, "view");
    SetMat4Identity(program, "projection");
    SetMat3Identity(program, "normalMatrix");
    glUniform3f(glGetUniformLocation(program, "lightPos"), 0.0f, 0.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "viewPos"), 0.0f, 0.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 1.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "objectColor"), 0.2f, 0.7f, 0.3f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::array<unsigned char, 4> center_pixel = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center_pixel.data());
    const int color_sum = center_pixel[0] + center_pixel[1] + center_pixel[2];
    EXPECT_GT(color_sum, 10) << "center pixel was effectively black";

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color_texture);
    glDeleteProgram(program);
}

TEST(RenderSmokeTest, RenderPipelineHotPathLogsAreCounterBacked) {
    const std::string source = ReadRenderPipelineCombinedSources();
    ASSERT_FALSE(source.empty());

    const std::vector<std::string> forbidden_hot_path_messages = {
        "CAMERA DEBUG",
        "RENDER DEBUG",
        "MESH UPLOAD:"
    };

    for (const std::string& message : forbidden_hot_path_messages) {
        EXPECT_EQ(source.find(message), std::string::npos) << message;
    }
}

TEST(RenderSmokeTest, RenderPipelineExposesPassBudgetCounters) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadRenderPipelineCombinedSources();
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("RenderPassFrameStats"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_stats"), std::string::npos);
    EXPECT_NE(header.find("shadow_cascade_draws"), std::string::npos);
    // T-I3-9: far-LOD region draws are counter-backed like every other pass.
    EXPECT_NE(header.find("far_region_draws"), std::string::npos);
    EXPECT_NE(header.find("far_indices_drawn"), std::string::npos);
    EXPECT_NE(source.find("ensure_terrain_culling_hierarchy"), std::string::npos);
    EXPECT_NE(source.find("shadow_cascade_visible_chunks"), std::string::npos);

    fs::create_directories(RenderPerfArtifactRoot());
    std::ofstream output(RenderPerfArtifactRoot() / "pass_counts.json");
    ASSERT_TRUE(output);
    output << "{\n";
    output << "  \"counter_contract\": {\n";
    output << "    \"terrain_draws\": true,\n";
    output << "    \"terrain_visible_chunks\": true,\n";
    output << "    \"far_region_draws\": true,\n";
    output << "    \"far_indices_drawn\": true,\n";
    output << "    \"culling_hierarchy_rebuilds\": true,\n";
    output << "    \"shadow_cascade_draws\": true,\n";
    output << "    \"shadow_cascade_visible_chunks\": true,\n";
    output << "    \"ssao_draws\": true,\n";
    output << "    \"lighting_draws\": true,\n";
    output << "    \"water_draws\": true,\n";
    output << "    \"skybox_draws\": true,\n";
    output << "    \"final_blits\": true\n";
    output << "  }\n";
    output << "}\n";
}

TEST(RenderSmokeTest, RenderFrameworkContractsEmitArtifacts) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadRenderPipelineCombinedSources();
    const std::string shader_header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.h");
    const std::string shader_source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.cpp");
    const std::string capture_header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.h");
    const std::string capture_source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(shader_header.empty());
    ASSERT_FALSE(shader_source.empty());
    ASSERT_FALSE(capture_header.empty());
    ASSERT_FALSE(capture_source.empty());

    EXPECT_NE(header.find("RenderPassMetadata"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_metadata"), std::string::npos);
    EXPECT_NE(header.find("RenderResourceRegistryStats"), std::string::npos);
    EXPECT_NE(header.find("get_resource_registry_stats"), std::string::npos);
    EXPECT_NE(header.find("ShaderHealthEntry"), std::string::npos);
    EXPECT_NE(header.find("get_shader_health"), std::string::npos);
    EXPECT_NE(source.find("refresh_render_pass_metadata"), std::string::npos);
    EXPECT_NE(source.find("destroy_lighting_fbo"), std::string::npos);
    EXPECT_NE(source.find("glObjectLabel"), std::string::npos);
    EXPECT_NE(source.find("make_terrain_fallback_texture"), std::string::npos);
    EXPECT_NE(source.find("terrain_texture_fallback_layers"), std::string::npos);
    EXPECT_NE(source.find("copy_lighting_color_to_opaque_texture"), std::string::npos);
    EXPECT_NE(source.find("lighting.opaque_color_copy"), std::string::npos);
    EXPECT_NE(source.find("u_causticsTexture"), std::string::npos);
    EXPECT_NE(source.find("u_normal_map"), std::string::npos);
    EXPECT_NE(source.find("u_flow_map"), std::string::npos);
    EXPECT_NE(source.find("u_foam_texture"), std::string::npos);
    EXPECT_NE(source.find("u_underwater_texture"), std::string::npos);
    // T-I4-7: triplanar terrain albedo + normal mapping moved from the lighting
    // pass into the G-buffer pass (the textured albedo and normal-mapped normal
    // are baked into the G-buffer). The contract now lives in g_buffer.frag.
    {
        const std::string gbuffer_frag = ReadTextFile(SourceRoot() / "res/shaders/g_buffer.frag");
        EXPECT_NE(gbuffer_frag.find("u_terrainTextures"), std::string::npos);
        EXPECT_NE(gbuffer_frag.find("u_terrainNormals"), std::string::npos);
        EXPECT_NE(gbuffer_frag.find("triplanar_albedo"), std::string::npos);
        EXPECT_NE(gbuffer_frag.find("triplanar_normal"), std::string::npos);
    }
    EXPECT_NE(shader_header.find("Diagnostic"), std::string::npos);
    EXPECT_NE(shader_source.find("m_valid = true"), std::string::npos);
    EXPECT_NE(capture_header.find("RenderDoc"), std::string::npos);
    EXPECT_NE(capture_header.find("PIX"), std::string::npos);
    EXPECT_NE(capture_header.find("Nsight"), std::string::npos);
    EXPECT_NE(capture_source.find("luminumbra.capture.ready"), std::string::npos);

    fs::create_directories(RenderFrameworkArtifactRoot());

    std::ofstream pass_metadata(RenderFrameworkArtifactRoot() / "render_pass_metadata.json");
    ASSERT_TRUE(pass_metadata);
    pass_metadata << "{\n";
    pass_metadata << "  \"schema\": \"luminumbra.render_framework.pass_metadata.v1\",\n";
    pass_metadata << "  \"passes\": [\n";
    pass_metadata << "    {\"name\":\"shadow\",\"inputs\":[\"terrain_depth\"],\"outputs\":[\"shadow.depth_texture_array\"],\"resolution\":\"shadow_map\",\"clear\":\"depth\",\"load_store\":\"store depth cascades\",\"draw_count_source\":\"shadow_draws\"},\n";
    pass_metadata << "    {\"name\":\"gbuffer\",\"inputs\":[\"terrain_meshes\",\"static_meshes\",\"material_lut\",\"terrain_texture_array\",\"terrain_normal_array\"],\"outputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"gbuffer.albedo_roughness\",\"gbuffer.metallic_ao\",\"gbuffer.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store deferred attachments\",\"draw_count_source\":\"terrain_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao\",\"inputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"ssao.noise\"],\"outputs\":[\"ssao.raw\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store ambient occlusion\",\"draw_count_source\":\"ssao_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao_blur\",\"inputs\":[\"ssao.raw\"],\"outputs\":[\"ssao.blur\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store blurred ambient occlusion\",\"draw_count_source\":\"ssao_blur_draws\"},\n";
    pass_metadata << "    {\"name\":\"lighting\",\"inputs\":[\"gbuffer.*\",\"shadow.depth_texture_array\",\"ssao.blur\",\"terrain_texture_array\",\"material_lut\",\"water.fallback.black\"],\"outputs\":[\"lighting.color\",\"lighting.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store lit scene\",\"draw_count_source\":\"lighting_draws\"},\n";
    pass_metadata << "    {\"name\":\"water\",\"inputs\":[\"lighting.opaque_color_copy\",\"gbuffer.depth\",\"water_meshes\",\"water.fallback.*\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"blend water into lighting\",\"draw_count_source\":\"water_draws\"},\n";
    pass_metadata << "    {\"name\":\"skybox\",\"inputs\":[\"skybox_vertices\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"store sky contribution\",\"draw_count_source\":\"skybox_draws\"},\n";
    pass_metadata << "    {\"name\":\"particles\",\"inputs\":[\"particle_instances\",\"gbuffer.depth\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"blend forward-lit particles\",\"draw_count_source\":\"particle_draws\"},\n";
    pass_metadata << "    {\"name\":\"final_blit\",\"inputs\":[\"lighting.color\"],\"outputs\":[\"swapchain.color\"],\"resolution\":\"screen\",\"clear\":\"default color+depth\",\"load_store\":\"present-ready color\",\"draw_count_source\":\"final_blits\"}\n";
    pass_metadata << "  ]\n";
    pass_metadata << "}\n";

    std::ofstream resource_registry(RenderFrameworkArtifactRoot() / "resource_registry.json");
    ASSERT_TRUE(resource_registry);
    resource_registry << "{\n";
    resource_registry << "  \"schema\": \"luminumbra.render_framework.resource_registry.v1\",\n";
    resource_registry << "  \"debug_labels\": true,\n";
    resource_registry << "  \"resource_types\": [\"framebuffer\", \"texture\", \"renderbuffer\", \"buffer\", \"vertex_array\", \"shader_program\"],\n";
    resource_registry << "  \"resize_recreates\": [\"lighting.fbo\", \"gbuffer.fbo\", \"ssao.fbo\", \"ssao.blur_fbo\"],\n";
    resource_registry << "  \"shutdown_requires_empty_registry\": true\n";
    resource_registry << "}\n";

    std::ofstream terrain_diagnostics(RenderFrameworkArtifactRoot() / "terrain_material_diagnostics.json");
    ASSERT_TRUE(terrain_diagnostics);
    terrain_diagnostics << "{\n";
    terrain_diagnostics << "  \"schema\": \"luminumbra.render_framework.terrain_materials.v1\",\n";
    terrain_diagnostics << "  \"texture_array\": \"terrain.texture_array\",\n";
    terrain_diagnostics << "  \"material_lut\": \"terrain.material_lut\",\n";
    terrain_diagnostics << "  \"material_registry\": \"data/common/materials.json\",\n";
    terrain_diagnostics << "  \"missing_texture_behavior\": \"visible magenta checker fallback\",\n";
    terrain_diagnostics << "  \"fallback_counter\": \"terrain_texture_fallback_layers\",\n";
    terrain_diagnostics << "  \"production_requires_zero_fallback_layers\": true,\n";
    terrain_diagnostics << "  \"terrain_texture_layers\": 5\n";
    terrain_diagnostics << "}\n";

    std::ofstream shader_health(RenderFrameworkArtifactRoot() / "shader_health.json");
    ASSERT_TRUE(shader_health);
    shader_health << "{\n";
    shader_health << "  \"schema\": \"luminumbra.render_framework.shader_health.v1\",\n";
    shader_health << "  \"runtime_validity_requires_compile_and_link_success\": true,\n";
    shader_health << "  \"programs\": [\"basic\", \"g_buffer\", \"instanced_mesh_gbuffer\", \"lighting_pass\", \"skybox\", \"shadow_map\", \"ssao\", \"ssao_blur\", \"water\", \"rml_ui\", \"loading_hologram\", \"loading_visual\", \"volumetric_lighting\", \"magical_particles\"]\n";
    shader_health << "}\n";

    std::ofstream capture_hooks(RenderFrameworkArtifactRoot() / "capture_hooks.json");
    ASSERT_TRUE(capture_hooks);
    capture_hooks << "{\n";
    capture_hooks << "  \"schema\": \"luminumbra.render_framework.capture_hooks.v1\",\n";
    capture_hooks << "  \"primary_backend\": \"RenderDoc\",\n";
    capture_hooks << "  \"optional_backends\": [\"PIX\", \"Nsight\"],\n";
    capture_hooks << "  \"marker_prefix\": \"luminumbra.capture.ready\",\n";
    capture_hooks << "  \"screenshot_artifact_dir\": \"render_captures\"\n";
    capture_hooks << "}\n";
}
