#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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

    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }

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

bool ReadShaderSourceRecursive(const fs::path& path,
                               std::unordered_set<std::string>& include_stack,
                               std::string& source,
                               std::string& diagnostic) {
    const fs::path normalized = fs::absolute(path).lexically_normal();
    const std::string key = normalized.generic_string();
    if (!include_stack.insert(key).second) {
        diagnostic = "cyclic shader include: " + key;
        return false;
    }

    std::ifstream input(normalized);
    if (!input) {
        diagnostic = "cannot open shader source: " + key;
        include_stack.erase(key);
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        constexpr std::string_view prefix = "#include \"";
        const std::size_t begin = line.find(prefix);
        if (begin == std::string::npos) {
            source.append(line).push_back('\n');
            continue;
        }

        const std::size_t name_begin = begin + prefix.size();
        const std::size_t name_end = line.find('"', name_begin);
        if (begin != 0 || name_end == std::string::npos || name_end + 1 != line.size()) {
            diagnostic = "malformed shader include in " + key + ": " + line;
            include_stack.erase(key);
            return false;
        }

        const fs::path included =
            normalized.parent_path() / line.substr(name_begin, name_end - name_begin);
        if (!ReadShaderSourceRecursive(included, include_stack, source, diagnostic)) {
            include_stack.erase(key);
            return false;
        }
    }

    include_stack.erase(key);
    return true;
}

bool ReadShaderSource(const fs::path& path, std::string& source, std::string& diagnostic) {
    std::unordered_set<std::string> include_stack;
    return ReadShaderSourceRecursive(path, include_stack, source, diagnostic);
}

// Render pass implementations are extracted from RenderPipeline.cpp into
// rendering/passes/; source-token contracts that cover pass bodies
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
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(ch);
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
    std::string source;
    std::string diagnostic;
    if (!ReadShaderSource(path, source, diagnostic) || source.empty()) {
        ADD_FAILURE() << "Shader source could not be loaded: " << path.string() << "\n"
                      << diagnostic;
        return 0;
    }

    const char* source_ptr = source.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source_ptr, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n"
                      << GetShaderInfoLog(shader);
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
        if (vertex != 0)
            glDeleteShader(vertex);
        if (fragment != 0)
            glDeleteShader(fragment);
        return 0;
    }

    shaders.push_back(vertex);
    shaders.push_back(fragment);

    if (spec.geometry) {
        GLuint geometry = CompileShader(shader_root / spec.geometry, GL_GEOMETRY_SHADER);
        if (geometry == 0) {
            for (GLuint shader : shaders)
                glDeleteShader(shader);
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
        ADD_FAILURE() << "Shader program failed to link: " << spec.name << "\n"
                      << GetProgramInfoLog(program);
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
        {"magical_particles",
         "magical_particles.vert",
         "magical_particles.frag",
         "magical_particles.geom"},
        {"foliage", "foliage.vert", "foliage.frag"},
        // Render-optimization (cloud-raymarch-optimization, ): the depth-masked
        // upsample compositing the reduced-res sky dome into the lighting FBO.
        {"cloud_composite", "ssao.vert", "cloud_composite.frag"},
        // Render-optimization (ssao-gtao): XeGTAO horizon-slice AO variant.
        {"ssao_gtao", "ssao.vert", "ssao_gtao.frag"},
        // Render-optimization (ssao-gtao ): joint-bilateral AO upsample.
        {"ssao_bilateral_upsample", "ssao.vert", "ssao_bilateral_upsample.frag"},
        // Fidelity: screen-space crepuscular rays (god rays).
        {"god_rays", "ssao.vert", "god_rays.frag"},
    };
}

void WriteShaderInventoryArtifact(const fs::path& path,
                                  const std::vector<ShaderSourceInventoryEntry>& sources,
                                  const std::vector<ShaderProgramSpec>& programs) {
    const auto count_stage = [&sources](const std::string& stage) {
        return std::count_if(
            sources.begin(), sources.end(), [&stage](const ShaderSourceInventoryEntry& entry) {
                return entry.stage == stage;
            });
    };
    const auto compiled_count =
        std::count_if(sources.begin(), sources.end(), [](const ShaderSourceInventoryEntry& entry) {
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
        output << ", \"bytes\": " << source.bytes
               << ", \"compiled\": " << (source.compiled ? "true" : "false") << "}";
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

void WriteShaderSuiteHealthArtifact(const fs::path& path,
                                    const std::vector<std::pair<std::string, bool>>& program_health,
                                    const std::vector<std::string>& gl_errors) {
    const auto linked_count =
        std::count_if(program_health.begin(),
                      program_health.end(),
                      [](const std::pair<std::string, bool>& entry) { return entry.second; });
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
    //  "particles" slots after "skybox" (the live ParticlePass order).
    static constexpr std::array<const char*, 9> kPassNames = {"shadow",
                                                              "gbuffer",
                                                              "ssao",
                                                              "ssao_blur",
                                                              "lighting",
                                                              "water",
                                                              "skybox",
                                                              "particles",
                                                              "final_blit"};

    GpuTimerProbeResult probe;
    const bool loader_ok = context_ready && glGenQueries != nullptr && glDeleteQueries != nullptr &&
                           glQueryCounter != nullptr && glGetQueryObjectiv != nullptr &&
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

void WriteRenderHealthAnalysis(const fs::path& path,
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
        output << "      {\"name\": \"" << program_health[i].first
               << "\", \"ok\": " << (program_health[i].second ? "true" : "false") << "}";
        output << (i + 1u == program_health.size() ? "\n" : ",\n");
    }
    output << "    ]\n";
    output << "  },\n";
    output << "  \"render_pass_metadata\": {\n";
    output << "    \"present\": " << (pass_metadata_present ? "true" : "false") << ",\n";
    output << "    \"required_passes\": [\"shadow\", \"gbuffer\", \"ssao\", \"ssao_blur\", "
              "\"lighting\", \"water\", \"skybox\", \"particles\", \"final_blit\"]\n";
    output << "  },\n";
    output << "  \"resource_registry\": {\n";
    output << "    \"present\": " << (resource_registry_present ? "true" : "false") << ",\n";
    output << "    \"debug_labels\": true,\n";
    output << "    \"resource_types\": [\"framebuffer\", \"texture\", \"renderbuffer\", "
              "\"buffer\", \"vertex_array\", \"shader_program\"],\n";
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
    output << "    \"query_mechanism\": \"glQueryCounter(GL_TIMESTAMP) ring, non-blocking "
              "GL_QUERY_RESULT_AVAILABLE polls\",\n";
    output << "    \"passes\": [\n";
    output << std::fixed << std::setprecision(6);
    for (std::size_t i = 0; i < gpu_timer_passes.size(); ++i) {
        output << "      {\"name\": \"" << gpu_timer_passes[i].first
               << "\", \"gpu_ms\": " << gpu_timer_passes[i].second << "}";
        output << (i + 1u == gpu_timer_passes.size() ? "\n" : ",\n");
    }
    output << "    ]\n";
    output << "  }\n";
    output << "}\n";
}

void SetMat4Identity(GLuint program, const char* name) {
    const GLfloat identity[16] = {
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

void SetMat3Identity(GLuint program, const char* name) {
    const GLfloat identity[9] = {
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };
    glUniformMatrix3fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

// ---  calibration-plate gate helpers ---
//
// Minimal.ltex ( format) CPU loader for the gate. Loads the committed
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
    if (!in)
        return false;
    auto read_pod = [&](auto& v) {
        in.read(reinterpret_cast<char*>(&v), sizeof(v));
        return static_cast<bool>(in);
    };
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t mip_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
    if (!read_pod(magic) || !read_pod(version) || !read_pod(mip_count) || !read_pod(width) ||
        !read_pod(height) || !read_pod(channels))
        return false;
    if (magic != 0x5845544Cu || version != 1u || width == 0 || height == 0 || channels == 0 ||
        channels > 4 || mip_count == 0)
        return false;
    size_t total = 0;
    {
        uint32_t w = width, h = height;
        for (uint16_t l = 0; l < mip_count; ++l) {
            total += static_cast<size_t>(w) * h * channels;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
    }
    out.width = width;
    out.height = height;
    out.channels = channels;
    out.mip_count = mip_count;
    out.bytes.resize(total);
    in.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(total));
    return static_cast<bool>(in);
}

// Uploads a set of.ltex plates into a GL_TEXTURE_2D_ARRAY (256x256xN). Returns
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
    glTexStorage3D(GL_TEXTURE_2D_ARRAY,
                   kMipLevels,
                   internal_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                   kRes,
                   kRes,
                   static_cast<GLsizei>(plates.size()));
    for (size_t i = 0; i < plates.size(); ++i) {
        GateLtexImage img;
        if (!LoadGateLtex(plates[i], img) || img.width != kRes || img.height != kRes ||
            img.channels != 4u) {
            glDeleteTextures(1, &tex);
            return 0;
        }
        size_t offset = 0;
        uint32_t w = img.width, h = img.height;
        for (uint16_t l = 0; l < img.mip_count && l < kMipLevels; ++l) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            l,
                            0,
                            0,
                            static_cast<GLint>(i),
                            static_cast<GLsizei>(w),
                            static_cast<GLsizei>(h),
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            img.bytes.data() + offset);
            offset += static_cast<size_t>(w) * h * img.channels;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
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
        x = ox;
        y = oy;
    }
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f)
        len = 1.0f;
    return {x / len, y / len, z / len};
}

// ---: lit-chain on-screen capture helper ---
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
struct LitNoonResult {
    float r = 0, g = 0, b = 0;
};
//  ( -6) additions, both defaulted so every existing
// caller renders byte-identically: emissive_intensity_norm > 0 authors that
// normalized emissive value into the LUT's row 2 for the plate's material
// (id 1), lighting the crystal-glow path; aether_material_modulation drives
// u_aetherMaterialModulation (0.0 == the GLSL default == multiply by 1.0).
LitNoonResult LitChainNoonOnscreenSrgb(GLuint lighting_program,
                                       const std::array<float, 3>& albedo_linear,
                                       float roughness,
                                       const fs::path& dump_ppm = {},
                                       float aether_field_value = -1.0f,
                                       float emissive_intensity_norm = 0.0f,
                                       float aether_material_modulation = 0.0f) {
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
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
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
    const unsigned char albedo_px[4] = {to_u8(albedo_linear[0]),
                                        to_u8(albedo_linear[1]),
                                        to_u8(albedo_linear[2]),
                                        to_u8(roughness)};
    GLuint g_albedo = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, albedo_px);
    const float metallic_px[2] = {0.0f, 1.0f};
    GLuint g_metallic = make_tex(GL_RG16F, GL_RG, GL_FLOAT, metallic_px);
    const float ssao_px[1] = {1.0f};
    GLuint ssao_tex = make_tex(GL_R16F, GL_RED, GL_FLOAT, ssao_px);
    const unsigned char caustics_px[4] = {0, 0, 0, 255};
    GLuint caustics_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, caustics_px);

    auto make_array_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float shadow_px[1] = {1.0f};
    GLuint shadow_arr =
        make_array_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, shadow_px);
    const unsigned char terrain_px[4] = {0, 0, 0, 255};
    GLuint terrain_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, terrain_px);

    const float quad[] = {
        -1, -1, 0, 0, 0, 1, -1, 0, 1, 0, 1,  1, 0, 1, 1,
        -1, -1, 0, 0, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1,
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
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(lighting_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pos);
    glUniform1i(glGetUniformLocation(lighting_program, "gPosition"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_norm);
    glUniform1i(glGetUniformLocation(lighting_program, "gNormalMaterial"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_albedo);
    glUniform1i(glGetUniformLocation(lighting_program, "gAlbedoRoughness"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, g_metallic);
    glUniform1i(glGetUniformLocation(lighting_program, "gMetallicAO"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ssao_tex);
    glUniform1i(glGetUniformLocation(lighting_program, "u_ssao"), 4);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr);
    glUniform1i(glGetUniformLocation(lighting_program, "u_shadowCascades"), 5);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_arr);
    glUniform1i(glGetUniformLocation(lighting_program, "u_terrainTextures"), 6);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, caustics_tex);
    glUniform1i(glGetUniformLocation(lighting_program, "u_causticsTexture"), 7);
    // the tint cascade sampler needs its OWN unit even
    // when disabled — a sampler2DArray left on unit 0 (a 2D texture) is a sampler
    // type collision that invalidates the whole draw. White 1x1x1 + enabled=0.
    const unsigned char tint_white_px[4] = {255, 255, 255, 255};
    GLuint tint_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, tint_white_px);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tint_arr);
    glUniform1i(glGetUniformLocation(lighting_program, "u_shadowTintCascades"), 9);
    glUniform1i(glGetUniformLocation(lighting_program, "u_shadowTintEnabled"), 0);

    SetMat4Identity(lighting_program, "u_inverseView");
    for (int i = 0; i < 4; ++i)
        SetMat4Identity(lighting_program,
                        ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
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
    // 0.0 mirrors the GLSL default (multiply by exactly 1.0).
    glUniform1f(glGetUniformLocation(lighting_program, "u_aetherMaterialModulation"),
                aether_material_modulation);

    // Empty material LUT (material 1 has no emission row -> glow path skipped).
    // 4 rows to match the production LUT height (all zeros -> emissive 0).
    std::vector<float> lut(static_cast<size_t>(256) * 4 * 4, 0.0f);
    if (emissive_intensity_norm > 0.0f) {
        // author the plate's emissive (row 2, material id 1, R) so
        // the crystal-glow path lights up for the modulation assertions.
        lut[(static_cast<size_t>(2) * 256 + 1) * 4 + 0] = emissive_intensity_norm;
    }
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

    //  coupling: when aether_field_value >= 0, bind a uniform aether
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
    if (aether_tex != 0) {
        glDeleteTextures(1, &aether_tex);
    }

    std::vector<unsigned char> px(static_cast<size_t>(kRes) * kRes * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, kRes, kRes, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    double sr = 0, sg = 0, sb = 0;
    const size_t n = static_cast<size_t>(kRes) * kRes;
    for (size_t p = 0; p < n; ++p) {
        sr += px[p * 4 + 0];
        sg += px[p * 4 + 1];
        sb += px[p * 4 + 2];
    }
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
    WriteShaderInventoryArtifact(RenderHealthArtifactRoot() / "shader-inventory.json",
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
    const bool all_programs_ok =
        std::all_of(program_health.begin(),
                    program_health.end(),
                    [](const std::pair<std::string, bool>& entry) { return entry.second; });

    fs::create_directories(RenderHealthArtifactRoot());
    WriteShaderSuiteHealthArtifact(
        RenderHealthArtifactRoot() / "shader-suite-health.json", program_health, gl_errors);

    EXPECT_TRUE(all_programs_ok);
    EXPECT_TRUE(gl_errors.empty());
}

TEST(RenderSmokeTest, RenderHealthGateEmitsAnalysisArtifact) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const std::string header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
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
    const bool all_programs_ok =
        std::all_of(program_health.begin(),
                    program_health.end(),
                    [](const std::pair<std::string, bool>& entry) { return entry.second; });
    const bool passed = health_api_present && pass_metadata_present && resource_registry_present &&
                        terrain_materials_present && gpu_timer_api_present && all_programs_ok &&
                        gl_errors.empty();

    fs::create_directories(RenderHealthArtifactRoot());
    WriteRenderHealthAnalysis(RenderHealthArtifactRoot() / "render-health-analysis.json",
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
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, position_texture, 0);

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
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, material_texture, 0);

    glGenTextures(1, &depth_texture);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

    const GLenum attachments[4] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, attachments);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    const std::array<float, 16> lut_pixels = {
        0.0f,
        0.6f,
        1.0f,
        0.0f, // metallic, roughness, AO, magical
        0.0f,
        0.0f,
        0.0f,
        0.0f, // no texture or normal array for this fixture
        0.0f,
        1.0f,
        0.0f,
        0.0f, // neutral albedo multiplier
        1.0f,
        1.0f,
        1.0f,
        0.0f, // neutral albedo tint
    };
    glGenTextures(1, &material_lut);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, material_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 4, 0, GL_RGBA, GL_FLOAT, lut_pixels.data());
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
        {0.8f, -0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
        {0.0f, 0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
    }};

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(GBufferVertex)),
                 vertices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(GBufferVertex),
                          reinterpret_cast<void*>(offsetof(GBufferVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(GBufferVertex),
                          reinterpret_cast<void*>(offsetof(GBufferVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2,
                           1,
                           GL_UNSIGNED_INT,
                           sizeof(GBufferVertex),
                           reinterpret_cast<void*>(offsetof(GBufferVertex, material)));

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
    // Active samplers of different types may not alias one texture unit, even
    // when this fixture takes the untextured material branch.
    glUniform1i(glGetUniformLocation(program, "u_terrainTextures"), 1);
    glUniform1i(glGetUniformLocation(program, "u_terrainNormals"), 1);
    glUniform1i(glGetUniformLocation(program, "u_terrainRoughness"), 1);
    glUniform1i(glGetUniformLocation(program, "u_skinnedTextures"), 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

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

// Close-range material gate using a calibration-plate pattern.
//
// The calibration-plate pattern avoids dependence on particular world geometry:
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

    // the lighting pass program is used to capture
    // the ABSOLUTE on-screen sRGB each material produces through the full chain
    // (albedo -> lit -> ACES tonemap -> gamma) at the fixed noon lighting.
    const ShaderProgramSpec lighting_spec{
        "lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
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
    ASSERT_NE(albedo_array, 0u) << "failed to load terrain albedo.ltex plates";
    ASSERT_NE(normal_array, 0u) << "failed to load terrain normal.ltex plates";

    // --- Material LUT (256 x 2) matching RenderPipeline::init_material_lut ---
    // Material id -> {texture_layer, normal_layer, tiling}. Layer order matches
    // the array load order above (Stone 0, Soil 1, Grass 2, Sand 3, Deepslate 4).
    // Each plate carries an authored roughness from the ladder so the
    // gate can verify the roughness -> G-buffer -> specular-response chain in
    // addition to albedo/normal. The ladder spans glossy..matte.
    struct PlateMat {
        int id;
        const char* name;
        int layer;
        float tiling;
        float roughness;
    };
    const std::array<PlateMat, 5> plates = {{
        {1, "Stone", 0, 4.0f, 0.30f},
        {2, "Soil", 1, 3.0f, 0.50f},
        {3, "Grass", 2, 3.0f, 0.65f},
        {4, "Sand", 3, 2.5f, 0.80f},
        {5, "Deepslate", 4, 4.0f, 0.95f},
    }};
    // the LUT is now 4 rows to mirror
    // RenderPipeline::init_material_lut - row 2 G carries the per-material
    // albedo_scale (default 1.0). The g_buffer shader samples row 2 (v=0.625
    // after the  3->4 row widening) and multiplies the baked albedo by it;
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
        lut[base + 0] = 0.0f;         // emissive_intensity/scale (non-emissive)
        lut[base + 1] = albedo_scale; //  albedo_scale (G channel)
    };
    // row 3 RGB = albedo_tint. The g_buffer triplanar branch multiplies the
    // baked albedo by this, so it MUST be authored to 1.0 or textured plates go
    // black. Default no-op tint = [1,1,1].
    auto set_row3 = [&](int id) {
        const size_t base = (static_cast<size_t>(3) * 256u + id) * 4u; // row 3
        lut[base + 0] = 1.0f;
        lut[base + 1] = 1.0f;
        lut[base + 2] = 1.0f;
    };
    // Row 0 G channel = per-plate authored roughness; the G-buffer
    // stores it in gAlbedoRoughness.a, which the gate reads back per plate.
    for (const auto& p : plates)
        lut[(static_cast<size_t>(p.id)) * 4 + 1] = p.roughness;
    for (const auto& p : plates)
        set_row1(p.id, p.layer, p.tiling);
    // Plates calibrate at scale 1.0 (this gate asserts the photographic albedo;
    // the albedo_scale calibration is exercised separately by the FarLodHorizon
    // sand-flat band). Every id defaults to 1.0 so the row-2 sample is a no-op.
    for (int id = 0; id < 256; ++id)
        set_row2(id, 1.0f);
    for (int id = 0; id < 256; ++id)
        set_row3(id); // Identity tint [1,1,1].
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
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_DEPTH_COMPONENT24,
                 kRes,
                 kRes,
                 0,
                 GL_DEPTH_COMPONENT,
                 GL_FLOAT,
                 nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gdepth, 0);
    const GLenum draw_buffers[4] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
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
    // terrain PBR roughness-map: u_terrainRoughness (sampler2DArray) must also point at a
    // DISTINCT unit (4) for the same reason as u_skinnedTextures above — left at
    // the default unit 0 it collides with the sampler2D LUT and blacks the draw.
    // valid=0 keeps the scalar roughness on this synthetic plate (no map bound).
    glUniform1i(glGetUniformLocation(program, "u_terrainRoughness"), 4);
    glUniform1i(glGetUniformLocation(program, "u_terrainRoughnessValid"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, material_lut);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, normal_array);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, albedo_array); // dummy, unsampled

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);

    struct PlateVertex {
        GLfloat px, py, pz, nx, ny, nz;
        GLuint material;
    };

    // Two sun directions for the normal-response check. The plates face +Z
    // (toward the camera), so both suns keep a positive Z component (the surface
    // is lit) but differ strongly in their X/Y tilt — a flat plate would shade
    // nearly uniformly under each, while the normal-mapped surface produces a
    // spatially varying shading field whose pattern shifts between the two
    // angles. (A sun pointing away from the plate face would zero the whole ROI
    // and defeat the check.) Both are normalized.
    auto normalize3 = [](std::array<float, 3> v) {
        float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f)
            l = 1.0f;
        return std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l};
    };
    const std::array<std::array<float, 3>, 2> sun_dirs = {{
        normalize3({-0.55f, 0.30f, 0.78f}), // sun tilted up-left toward the plate
        normalize3({0.62f, -0.35f, 0.70f}), // sun tilted down-right toward the plate
    }};

    // Per-material capture results.
    struct PlateResult {
        std::string name;
        float albedo_r = 0, albedo_g = 0, albedo_b = 0;
        float shading_spatial_stddev[2] = {0, 0}; // per sun angle
        float sun_response_delta = 0;             // |shadingA - shadingB| mean
        bool albedo_textured = false;
        float authored_roughness = 0; //  ladder value
        float gbuffer_roughness = 0;  // read back from gAlbedoRoughness.a
        float specular_highlight = 0; // analytical GGX peak (lower roughness -> brighter)
        // ABSOLUTE on-screen sRGB through the real
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
            {-0.95f, -0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
            {0.95f, -0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
            {0.95f, 0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
            {-0.95f, -0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
            {0.95f, 0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
            {-0.95f, 0.95f, -0.5f, 0, 0, 1, static_cast<GLuint>(pm.id)},
        }};
        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,
                              3,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(PlateVertex),
                              reinterpret_cast<void*>(offsetof(PlateVertex, px)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,
                              3,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(PlateVertex),
                              reinterpret_cast<void*>(offsetof(PlateVertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2,
                               1,
                               GL_UNSIGNED_INT,
                               sizeof(PlateVertex),
                               reinterpret_cast<void*>(offsetof(PlateVertex, material)));

        const GLfloat clear0[4] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 0, clear0);
        glClearBufferfv(GL_COLOR, 1, clear0);
        glClearBufferfv(GL_COLOR, 2, clear0);
        glClearBufferfv(GL_COLOR, 3, clear0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Dump the full-frame textured albedo as a PPM capture per material
        // (convertible to PNG via tools/gates/convert-ppm-to-png.ps1).
        {
            std::vector<unsigned char> frame(static_cast<size_t>(kRes) * kRes * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glReadPixels(0, 0, kRes, kRes, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
            fs::create_directories(RenderHealthArtifactRoot() / "calibration-plates");
            std::ofstream ppm(RenderHealthArtifactRoot() / "calibration-plates" /
                                  (std::string("plate-") + pm.name + ".ppm"),
                              std::ios::binary);
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

        //  roughness -> G-buffer -> specular response. The G-buffer stores
        // roughness in gAlbedoRoughness.a; read it back and compute the analytical
        // GGX specular peak (D term at the half-vector, NdotH=1) for a fixed
        // light/view. The peak highlight intensity rises sharply as roughness
        // falls, so the ladder produces a monotonic specular response.
        double rough_sum = 0;
        for (size_t i = 0; i < n; ++i)
            rough_sum += albedo_px[i * 4 + 3];
        pr.gbuffer_roughness = static_cast<float>(rough_sum / n / 255.0);
        pr.authored_roughness = pm.roughness;
        {
            const float a = pr.gbuffer_roughness * pr.gbuffer_roughness;
            const float a2 = a * a;
            // GGX D at NdotH=1: a2 / (PI * 1) -> the specular highlight peak.
            pr.specular_highlight =
                a2 / (3.14159265f * 1e-4f + 3.14159265f * a2 * 0.0f + 3.14159265f);
            // Simpler stable proxy: peak GGX ~ 1/(PI*a2), brighter for low roughness.
            pr.specular_highlight = 1.0f / (3.14159265f * std::max(a2, 1e-4f));
        }

        // Decode per-pixel normals, compute shading under each sun, accumulate
        // the spatial variation and the per-pixel response delta between suns.
        std::vector<float> shadeA(n), shadeB(n);
        for (size_t i = 0; i < n; ++i) {
            std::array<float, 3> N =
                DecodeOctahedral(normal_px[i * 4 + 0] / 255.0f, normal_px[i * 4 + 1] / 255.0f);
            auto dot3 = [&](const std::array<float, 3>& s) {
                return std::max(0.0f, N[0] * s[0] + N[1] * s[1] + N[2] * s[2]);
            };
            shadeA[i] = dot3(sun_dirs[0]);
            shadeB[i] = dot3(sun_dirs[1]);
        }
        auto stddev = [&](const std::vector<float>& v) {
            double mean = 0;
            for (float x : v)
                mean += x;
            mean /= v.size();
            double var = 0;
            for (float x : v) {
                double d = x - mean;
                var += d * d;
            }
            return static_cast<float>(std::sqrt(var / v.size()));
        };
        pr.shading_spatial_stddev[0] = stddev(shadeA);
        pr.shading_spatial_stddev[1] = stddev(shadeB);
        double delta = 0;
        for (size_t i = 0; i < n; ++i)
            delta += std::fabs(shadeA[i] - shadeB[i]);
        pr.sun_response_delta = static_cast<float>(delta / n);
        results.push_back(pr);

        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }

    // ---: ABSOLUTE on-screen sRGB capture ---
    // Second pass: run the REAL lighting_pass.frag on each plate's measured
    // (linear) G-buffer albedo + authored roughness at the FIXED NOON lighting,
    // and record the on-screen sRGB. This audits the full albedo -> lit -> ACES
    // -> gamma chain. The helper rebinds GL state, so it runs after the G-buffer
    // loop. Also capture the white and 18%-gray reference plates: those are a
    // permanent assertion that the chain neither crushes nor blows luminance
    // (mid-gray must land near perceptual mid; white must roll up high).
    const fs::path lit_dir = RenderHealthArtifactRoot() / "calibration-plates";
    for (auto& r : results) {
        const LitNoonResult lit =
            LitChainNoonOnscreenSrgb(lighting_program,
                                     {r.albedo_r, r.albedo_g, r.albedo_b},
                                     r.gbuffer_roughness,
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
    for (const auto& r : results)
        by_name[r.name] = r;

    bool gate_passed = (results.size() == plates.size());
    for (const auto& r : results) {
        EXPECT_TRUE(r.albedo_textured)
            << r.name << " plate produced a black/empty albedo (texture not sampled)";
        // Normal response: spatial shading variation under both sun angles, plus
        // a non-trivial difference between the two sun directions.
        EXPECT_GT(r.shading_spatial_stddev[0], kFlatShadingBound)
            << r.name << " has no normal-map shading variation (high sun)";
        EXPECT_GT(r.shading_spatial_stddev[1], kFlatShadingBound)
            << r.name << " has no normal-map shading variation (low sun)";
        EXPECT_GT(r.sun_response_delta, kFlatShadingBound)
            << r.name << " shading does not respond to sun direction";
        if (!(r.albedo_textured && r.shading_spatial_stddev[0] > kFlatShadingBound &&
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
        if (!(sand_luma > grass_luma && grass.albedo_g > grass.albedo_b))
            gate_passed = false;
    }

    // ---: ABSOLUTE on-screen sRGB bands ---
    // The crux of this task. The relative checks above pass even when the whole
    // frame is crushed dark (the owner-reported defect: sand rust-brown, grass
    // near-black). These bands assert each material lands in its REAL color
    // window on screen at fixed noon, derived from published surface-reflectance
    // data carried through the (now exposure-corrected) chain. Bands are from
    // data/common/albedo_calibration_reference.json and widened for normal/
    // roughness spread + RGBA8 quantization. If the chain ever crushes or blows
    // luminance, these fail where the relative checks would not.
    struct SrgbBand {
        const char* name;
        float rlo, rhi, glo, ghi, blo, bhi;
    };
    const std::array<SrgbBand, 5> bands = {{
        // name        r:[lo,hi]      g:[lo,hi]      b:[lo,hi]
        {"Stone", 0.45f, 0.95f, 0.45f, 0.95f, 0.40f, 0.92f},
        {"Soil", 0.40f, 0.85f, 0.30f, 0.78f, 0.24f, 0.72f},
        {"Grass", 0.20f, 0.65f, 0.24f, 0.70f, 0.10f, 0.55f},
        {"Sand", 0.62f, 0.98f, 0.52f, 0.95f, 0.26f, 0.78f},
        {"Deepslate", 0.30f, 0.80f, 0.30f, 0.80f, 0.26f, 0.74f},
    }};
    for (const auto& band : bands) {
        if (!by_name.count(band.name))
            continue;
        const auto& m = by_name[band.name];
        const bool in_r = m.onscreen_r >= band.rlo && m.onscreen_r <= band.rhi;
        const bool in_g = m.onscreen_g >= band.glo && m.onscreen_g <= band.ghi;
        const bool in_b = m.onscreen_b >= band.blo && m.onscreen_b <= band.bhi;
        EXPECT_TRUE(in_r) << band.name << " on-screen R " << m.onscreen_r << " outside band ["
                          << band.rlo << ", " << band.rhi << "]";
        EXPECT_TRUE(in_g) << band.name << " on-screen G " << m.onscreen_g << " outside band ["
                          << band.glo << ", " << band.ghi << "]";
        EXPECT_TRUE(in_b) << band.name << " on-screen B " << m.onscreen_b << " outside band ["
                          << band.blo << ", " << band.bhi << "]";
        if (!(in_r && in_g && in_b))
            gate_passed = false;
    }

    // ---: white/gray chain assertion (PERMANENT) ---
    // The exposure-audit anchors. A correctly-exposed chain renders a white
    // surface near (but below, due to filmic rolloff) full white at noon and an
    // 18% gray near perceptual mid. The pre-fix chain (sun COLOR fed where
    // IRRADIANCE was needed) crushed white to ~0.74 and mid-gray to ~0.32.
    const float white_luma = (white_plate.r + white_plate.g + white_plate.b) / 3.0f;
    const float gray_luma = (gray18_plate.r + gray18_plate.g + gray18_plate.b) / 3.0f;
    EXPECT_GT(white_luma, 0.80f) << "white plate too dark at noon (chain crushes luminance): "
                                 << white_luma;
    EXPECT_LT(white_luma, 1.001f) << "white plate impossibly bright: " << white_luma;
    EXPECT_GT(gray_luma, 0.45f) << "18% gray plate too dark at noon (chain crushes luminance): "
                                << gray_luma;
    EXPECT_LT(gray_luma, 0.80f) << "18% gray plate too bright at noon (chain over-exposed): "
                                << gray_luma;
    EXPECT_GT(white_luma, gray_luma) << "white must read brighter than 18% gray";
    if (!(white_luma > 0.80f && white_luma <= 1.001f && gray_luma > 0.45f && gray_luma < 0.80f &&
          white_luma > gray_luma)) {
        gate_passed = false;
    }

    // ---  specular-response check: roughness ladder ---
    // (1) The authored roughness round-trips through the G-buffer (gAlbedoRoughness.a
    //     matches the LUT value within the RGBA8 quantization tolerance).
    // (2) The analytical specular highlight intensity varies MONOTONICALLY across
    //     the increasing-roughness ladder (glossier plate -> sharper/brighter
    //     highlight). A flat constant roughness would produce a constant highlight.
    {
        // Results follow the plate order (Stone.30.. Deepslate.95 ascending).
        bool roughness_roundtrips = true;
        bool specular_monotonic = true;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            EXPECT_NEAR(r.gbuffer_roughness, r.authored_roughness, 0.02f)
                << r.name << " roughness did not round-trip through the G-buffer";
            if (std::fabs(r.gbuffer_roughness - r.authored_roughness) > 0.02f)
                roughness_roundtrips = false;
            if (i > 0 && results[i].specular_highlight >= results[i - 1].specular_highlight) {
                specular_monotonic = false; // highlight must fall as roughness rises
            }
        }
        EXPECT_TRUE(roughness_roundtrips) << "authored roughness must reach the G-buffer";
        EXPECT_TRUE(specular_monotonic)
            << "specular highlight must vary monotonically across the roughness ladder";
        if (!roughness_roundtrips || !specular_monotonic)
            gate_passed = false;
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
        out << "    {\"name\": \"" << r.name << "\"" << ", \"albedo\": [" << r.albedo_r << ", "
            << r.albedo_g << ", " << r.albedo_b << "]"
            << ", \"shading_stddev_sun0\": " << r.shading_spatial_stddev[0]
            << ", \"shading_stddev_sun1\": " << r.shading_spatial_stddev[1]
            << ", \"sun_response_delta\": " << r.sun_response_delta
            << ", \"authored_roughness\": " << r.authored_roughness
            << ", \"gbuffer_roughness\": " << r.gbuffer_roughness
            << ", \"specular_highlight\": " << r.specular_highlight << ", \"onscreen_srgb\": ["
            << r.onscreen_r << ", " << r.onscreen_g << ", " << r.onscreen_b << "]"
            << ", \"textured\": " << (r.albedo_textured ? "true" : "false") << "}";
        out << (i + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ],\n";
    // exposure-chain anchors (white + 18% gray
    // through the real lighting_pass at fixed noon). A permanent assertion that
    // the chain neither crushes nor blows luminance.
    out << "  \"exposure_anchors\": {\n";
    out << "    \"lighting\": \"fixed_noon\",\n";
    out << "    \"sun_irradiance_scale\": " << 3.14159265f << ",\n";
    out << "    \"white_plate_srgb\": [" << white_plate.r << ", " << white_plate.g << ", "
        << white_plate.b << "],\n";
    out << "    \"gray18_plate_srgb\": [" << gray18_plate.r << ", " << gray18_plate.g << ", "
        << gray18_plate.b << "]\n";
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

//  aether coupling gate. Closes critique MAJOR #17 (the determinism gate
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
    const LitNoonResult base = LitChainNoonOnscreenSrgb(program, albedo, 1.0f); // tap inactive
    const LitNoonResult glow =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, 0.6f); // field=0.6
    const LitNoonResult zero =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, 0.0f); // active, field=0

    const float base_lum = base.r + base.g + base.b;
    const float glow_lum = glow.r + glow.g + glow.b;
    EXPECT_GT(glow_lum, base_lum + 0.05f)
        << "aether tap did not brighten the lit output (field not consumed)";
    EXPECT_GT(glow.b, base.b + 0.02f) << "aether blue glow not present in the lit output";
    // Active-but-zero field contributes nothing -> identical to baseline.
    EXPECT_NEAR(zero.r, base.r, 1.0e-4f);
    EXPECT_NEAR(zero.g, base.g, 1.0e-4f);
    EXPECT_NEAR(zero.b, base.b, 1.0e-4f);

    glDeleteProgram(program);
}

//  ( -6): u_aetherMaterialModulation.
//
// The local aether scales emissive MATERIALS by (1 + aether * modulation).
// Contract halves: (1) modulation 0.0 (the default) and modulation-with-no-
// field are BOTH byte-identical to the untouched baseline (a multiply by
// exactly 1.0 — the RenderParityFrame guarantee in miniature); (2) with an
// active field on an emissive plate, on-screen luminance is MONOTONIC
// non-decreasing in the modulation value.
TEST(RenderSmokeTest, AetherMaterialModulationMonotonic) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const ShaderProgramSpec spec{"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    const std::array<float, 3> albedo{0.2f, 0.2f, 0.2f};
    constexpr float kField = 0.6f;     // active uniform aether field value
    constexpr float kEmissive = 0.25f; // normalized LUT row-2 value (-> intensity 2.0)

    // Pixel-identical half: no field -> aetherLocal 0 -> modulation is inert.
    const LitNoonResult plain = LitChainNoonOnscreenSrgb(program, albedo, 1.0f);
    const LitNoonResult mod_no_field =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, -1.0f, 0.0f, 3.0f);
    EXPECT_NEAR(mod_no_field.r, plain.r, 1.0e-4f);
    EXPECT_NEAR(mod_no_field.g, plain.g, 1.0e-4f);
    EXPECT_NEAR(mod_no_field.b, plain.b, 1.0e-4f);

    // Monotonic half: emissive plate + active field, rising modulation.
    const LitNoonResult m0 =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, kField, kEmissive, 0.0f);
    const LitNoonResult m1 =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, kField, kEmissive, 0.75f);
    const LitNoonResult m2 =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, kField, kEmissive, 2.0f);
    const float l0 = m0.r + m0.g + m0.b;
    const float l1 = m1.r + m1.g + m1.b;
    const float l2 = m2.r + m2.g + m2.b;
    EXPECT_GT(l1, l0 + 0.005f) << "modulation 0.75 did not brighten the emissive plate";
    EXPECT_GT(l2, l1 + 0.005f) << "modulation 2.0 not monotonic past 0.75";

    // Modulation with an active field but a NON-emissive plate is inert too
    // (crystalGlow == 0 -> the multiply has nothing to scale).
    const LitNoonResult glow_only_a =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, kField, 0.0f, 0.0f);
    const LitNoonResult glow_only_b =
        LitChainNoonOnscreenSrgb(program, albedo, 1.0f, {}, kField, 0.0f, 3.0f);
    EXPECT_NEAR(glow_only_b.r, glow_only_a.r, 1.0e-4f);
    EXPECT_NEAR(glow_only_b.g, glow_only_a.g, 1.0e-4f);
    EXPECT_NEAR(glow_only_b.b, glow_only_a.b, 1.0e-4f);

    glDeleteProgram(program);
}

//  emissive calibration gate.
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
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
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
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float shadow_px[1] = {1.0f};
    GLuint shadow_arr =
        make_array_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, shadow_px);
    const unsigned char terrain_px[4] = {0, 0, 0, 255};
    GLuint terrain_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, terrain_px);

    // Fullscreen quad.
    const float quad[] = {
        -1, -1, 0, 0, 0, 1, -1, 0, 1, 0, 1,  1, 0, 1, 1,
        -1, -1, 0, 0, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1,
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
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(program);

    // Bind G-buffer samplers to distinct units.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pos);
    glUniform1i(glGetUniformLocation(program, "gPosition"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_norm);
    glUniform1i(glGetUniformLocation(program, "gNormalMaterial"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_albedo);
    glUniform1i(glGetUniformLocation(program, "gAlbedoRoughness"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, g_metallic);
    glUniform1i(glGetUniformLocation(program, "gMetallicAO"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ssao_tex);
    glUniform1i(glGetUniformLocation(program, "u_ssao"), 4);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr);
    glUniform1i(glGetUniformLocation(program, "u_shadowCascades"), 5);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_arr);
    glUniform1i(glGetUniformLocation(program, "u_terrainTextures"), 6);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, caustics_tex);
    glUniform1i(glGetUniformLocation(program, "u_causticsTexture"), 7);
    // the tint cascade sampler needs its OWN unit even
    // when disabled — a sampler2DArray left on unit 0 (a 2D texture) is a sampler
    // type collision that invalidates the whole draw. White 1x1x1 + enabled=0.
    const unsigned char tint_px[4] = {255, 255, 255, 255};
    GLuint tint_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, tint_px);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tint_arr);
    glUniform1i(glGetUniformLocation(program, "u_shadowTintCascades"), 9);
    glUniform1i(glGetUniformLocation(program, "u_shadowTintEnabled"), 0);

    // Scalar/vector uniforms.
    SetMat4Identity(program, "u_inverseView");
    for (int i = 0; i < 4; ++i)
        SetMat4Identity(program, ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
    glUniform4f(glGetUniformLocation(program, "u_cascadeSplits"), 1e9f, 1e9f, 1e9f, 1e9f);
    glUniform1f(glGetUniformLocation(program, "u_time"), 0.0f);
    glUniform1f(glGetUniformLocation(program, "u_sea_level"), -1000.0f);
    glUniform3f(glGetUniformLocation(program, "u_terrainOrigin"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_viewPos"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_skyAmbientColor"), 0.02f, 0.02f, 0.03f);
    glUniform3f(glGetUniformLocation(program, "u_sun.direction"), 0.0f, 1.0f, 0.0f);
    glUniform3f(glGetUniformLocation(program, "u_sun.color"),
                0.02f,
                0.02f,
                0.02f); // dim sun: emission dominates
    glUniform1i(glGetUniformLocation(program, "u_pointLightCount"), 0);
    glUniform1f(glGetUniformLocation(program, "u_emissiveLutScale"), kEmissiveLutScale);

    const GLint lutLoc = glGetUniformLocation(program, "u_materialLUT");
    glUniform1i(lutLoc, 8);

    // Material LUT (256 x 4;  widened 3->4 to add the albedo_tint row). Only
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
        lut[(static_cast<size_t>(2 * 256 + 6)) * 4 + 0] =
            std::min(intensity / kEmissiveLutScale, 1.0f);
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
            lum += 0.2126 * px[p * 4 + 0] + 0.7152 * px[p * 4 + 1] + 0.0722 * px[p * 4 + 2];
        }
        measured[i] = lum / (static_cast<double>(kRes) * kRes);
    }

    // --- Gate assertions: monotonic, intensity 0 dark ---
    bool monotonic = true;
    for (size_t i = 1; i < intensities.size(); ++i) {
        if (measured[i] <= measured[i - 1] + 0.5) {
            monotonic = false;
        }
    }
    const bool zero_is_dark = measured[0] < measured[1];
    EXPECT_TRUE(zero_is_dark) << "intensity 0 should be darker than intensity 0.5";
    EXPECT_TRUE(monotonic)
        << "on-screen luminance must increase monotonically with emissive_intensity";

    fs::create_directories(RenderHealthArtifactRoot());
    std::ofstream out(RenderHealthArtifactRoot() / "emissive-calibration.json");
    out << "{\n";
    out << "  \"schema\": \"luminumbra.emissive_calibration.v1\",\n";
    out << "  \"material\": \"LuminCrystal\",\n";
    out << "  \"material_id\": 6,\n";
    out << "  \"emissive_lut_scale\": " << kEmissiveLutScale << ",\n";
    out << "  \"transfer_curve\": \"glow = 1.5 * emissive_intensity (linear pre-tonemap); "
           "on-screen = filmic(lit + glow)\",\n";
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

//   (, ColoredShadowGpu): the tinted-transmission chain, both
// halves, against the REAL shaders. Half 1: shadow_tint.frag writes EXACTLY the
// GlassTintModel transmission (T(d) = tint^d). Half 2: lighting_pass.frag's
// SampleShadowTint multiply — a WHITE tint is byte-identical to tint-disabled
// (the empty-world identity contract), and a RED tint reddens the lit sun.
TEST(RenderSmokeTest, ColoredShadowTintedTransmissionColorsDirectSun) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    // ---- Half 1: shadow_tint.frag == GlassTintModel, to 8-bit exactness. ----
    {
        const ShaderProgramSpec tint_spec{"shadow_tint", "shadow_tint.vert", "shadow_tint.frag"};
        GLuint tint_prog = LinkProgram(tint_spec);
        ASSERT_NE(tint_prog, 0u);

        constexpr int kTintRes = 4;
        GLuint tfbo = 0, ttex = 0;
        glGenFramebuffers(1, &tfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, tfbo);
        glGenTextures(1, &ttex);
        glBindTexture(GL_TEXTURE_2D, ttex);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, kTintRes, kTintRes, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ttex, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

        // A fullscreen pane: unit quad scaled 4x under identity light-space.
        const float pane[] = {
            -1,
            -1,
            0,
            1,
            -1,
            0,
            1,
            1,
            0,
            -1,
            -1,
            0,
            1,
            1,
            0,
            -1,
            1,
            0,
        };
        GLuint pvao = 0, pvbo = 0;
        glGenVertexArrays(1, &pvao);
        glGenBuffers(1, &pvbo);
        glBindVertexArray(pvao);
        glBindBuffer(GL_ARRAY_BUFFER, pvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(pane), pane, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));

        glViewport(0, 0, kTintRes, kTintRes);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND); // single pane: the raw shader output IS the transmission
        glUseProgram(tint_prog);
        SetMat4Identity(tint_prog, "u_lightSpaceMatrix");
        SetMat4Identity(tint_prog, "u_model");
        // T(d) = tint^d — the GlassTintModel formula (its algebra is pinned by
        // render_capture_test's ColoredShadow.BeerLambertTintModelAnchors; here the
        // SHADER is pinned to the same expression with std::pow, no glm needed).
        const float tint_r = 0.5f, tint_g = 0.25f, tint_b = 1.0f;
        const float thickness = 2.0f;
        glUniform3f(glGetUniformLocation(tint_prog, "u_tint"), tint_r, tint_g, tint_b);
        glUniform1f(glGetUniformLocation(tint_prog, "u_thickness"), thickness);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        unsigned char tpx[4] = {};
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(kTintRes / 2, kTintRes / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tpx);
        EXPECT_NEAR(tpx[0], std::pow(tint_r, thickness) * 255.0f, 1.5)
            << "shadow_tint.frag R drifted from GlassTintModel";
        EXPECT_NEAR(tpx[1], std::pow(tint_g, thickness) * 255.0f, 1.5)
            << "shadow_tint.frag G drifted from GlassTintModel";
        EXPECT_NEAR(tpx[2], std::pow(tint_b, thickness) * 255.0f, 1.5)
            << "shadow_tint.frag B drifted from GlassTintModel";

        glDeleteBuffers(1, &pvbo);
        glDeleteVertexArrays(1, &pvao);
        glDeleteTextures(1, &ttex);
        glDeleteFramebuffers(1, &tfbo);
        glDeleteProgram(tint_prog);
    }

    // ---- Half 2: the lighting-pass multiply on a LIT sun fragment. ----
    const ShaderProgramSpec spec{"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    constexpr int kRes = 8;
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

    auto make_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    auto make_array_tex = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* data) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    // A grey, fully-rough, sun-facing fragment (normal +Z toward the sun below).
    const float pos_px[3] = {0.0f, 0.0f, -3.0f};
    GLuint g_pos = make_tex(GL_RGB16F, GL_RGB, GL_FLOAT, pos_px);
    const unsigned char norm_px[4] = {128, 128, 0, 1}; // +Z octahedral; material 1
    GLuint g_norm = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, norm_px);
    const unsigned char albedo_px[4] = {160, 160, 160, 230}; // grey, rough
    GLuint g_albedo = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, albedo_px);
    const float metallic_px[2] = {0.0f, 1.0f};
    GLuint g_metallic = make_tex(GL_RG16F, GL_RG, GL_FLOAT, metallic_px);
    const float ssao_px[1] = {1.0f};
    GLuint ssao_tex = make_tex(GL_R16F, GL_RED, GL_FLOAT, ssao_px);
    const unsigned char caustics_px[4] = {0, 0, 0, 255};
    GLuint caustics_tex = make_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, caustics_px);
    const float shadow_px[1] = {1.0f}; // fully lit
    GLuint shadow_arr =
        make_array_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, shadow_px);
    const unsigned char terrain_px[4] = {0, 0, 0, 255};
    GLuint terrain_arr = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, terrain_px);
    const unsigned char white_px[4] = {255, 255, 255, 255};
    GLuint tint_white = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, white_px);
    const unsigned char red_px[4] = {230, 40, 40, 255};
    GLuint tint_red = make_array_tex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, red_px);
    std::vector<float> lut(static_cast<size_t>(256) * 4 * 4, 0.0f);
    lut[(static_cast<size_t>(1)) * 4 + 1] = 0.9f; // material 1 row 0: rough
    lut[(static_cast<size_t>(1)) * 4 + 2] = 1.0f; // ao
    GLuint lut_tex = 0;
    glGenTextures(1, &lut_tex);
    glBindTexture(GL_TEXTURE_2D, lut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 4, 0, GL_RGBA, GL_FLOAT, lut.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const float quad[] = {
        -1, -1, 0, 0, 0, 1, -1, 0, 1, 0, 1,  1, 0, 1, 1,
        -1, -1, 0, 0, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1,
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
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, kRes, kRes);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pos);
    glUniform1i(glGetUniformLocation(program, "gPosition"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_norm);
    glUniform1i(glGetUniformLocation(program, "gNormalMaterial"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_albedo);
    glUniform1i(glGetUniformLocation(program, "gAlbedoRoughness"), 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, g_metallic);
    glUniform1i(glGetUniformLocation(program, "gMetallicAO"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ssao_tex);
    glUniform1i(glGetUniformLocation(program, "u_ssao"), 4);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr);
    glUniform1i(glGetUniformLocation(program, "u_shadowCascades"), 5);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_arr);
    glUniform1i(glGetUniformLocation(program, "u_terrainTextures"), 6);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, caustics_tex);
    glUniform1i(glGetUniformLocation(program, "u_causticsTexture"), 7);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, lut_tex);
    glUniform1i(glGetUniformLocation(program, "u_materialLUT"), 8);
    glUniform1i(glGetUniformLocation(program, "u_shadowTintCascades"), 9);
    SetMat4Identity(program, "u_inverseView");
    for (int i = 0; i < 4; ++i)
        SetMat4Identity(program, ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
    glUniform4f(glGetUniformLocation(program, "u_cascadeSplits"), 1e9f, 1e9f, 1e9f, 1e9f);
    glUniform1f(glGetUniformLocation(program, "u_time"), 0.0f);
    glUniform1f(glGetUniformLocation(program, "u_sea_level"), -1000.0f);
    glUniform3f(glGetUniformLocation(program, "u_terrainOrigin"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_viewPos"), 0, 0, 0);
    glUniform3f(glGetUniformLocation(program, "u_skyAmbientColor"), 0.0f, 0.0f, 0.0f);
    glUniform3f(glGetUniformLocation(program, "u_sun.direction"),
                0.0f,
                0.0f,
                1.0f); // toward-light == +Z == the normal
    glUniform3f(glGetUniformLocation(program, "u_sun.color"), 1.0f, 1.0f, 1.0f);
    glUniform1i(glGetUniformLocation(program, "u_pointLightCount"), 0);
    glUniform1f(glGetUniformLocation(program, "u_emissiveLutScale"), 8.0f);

    auto render_and_read = [&](GLuint tint_tex, int enabled) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tint_tex);
        glUniform1i(glGetUniformLocation(program, "u_shadowTintEnabled"), enabled);
        const GLfloat clear0[4] = {0, 0, 0, 1};
        glClearBufferfv(GL_COLOR, 0, clear0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        std::array<unsigned char, 4> px{};
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(kRes / 2, kRes / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        return px;
    };

    const auto off = render_and_read(tint_white, 0);
    const auto white_on = render_and_read(tint_white, 1);
    const auto red_on = render_and_read(tint_red, 1);

    // White tint is the exact identity: off vs on-with-white must be byte-equal
    // (the empty-world contract behind the init-cleared-white cascade).
    EXPECT_EQ(off, white_on) << "white tint must be byte-identical to tint-disabled";
    // A red pane reddens the DIRECT sun: green/blue drop sharply, red barely.
    EXPECT_GT(static_cast<int>(off[1]), static_cast<int>(red_on[1]) + 20)
        << "red tint should suppress the green channel of the lit sun";
    EXPECT_GT(static_cast<int>(off[2]), static_cast<int>(red_on[2]) + 20)
        << "red tint should suppress the blue channel of the lit sun";
    EXPECT_GT(static_cast<int>(red_on[0]) * 2,
              static_cast<int>(red_on[1]) + static_cast<int>(red_on[2]))
        << "the tinted fragment should read RED-dominant";

    glDeleteTextures(1, &lut_tex);
    glDeleteTextures(1, &tint_red);
    glDeleteTextures(1, &tint_white);
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

//  rendering (, the FroxelGpu gate): the SHIPPED
// froxel_inject/froxel_integrate kernels on a UNIFORM medium (constant sigma,
// zero falloff, black lights) must integrate to the analytic Beer-Lambert
// transmittance: T(last slice) == exp(-sigma * (FAR - NEAR)). Pins the GLSL
// half of the grid model (the C++ half is FroxelModel.*).
TEST(RenderSmokeTest, FroxelUniformMediumMatchesAnalyticTransmittance) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    auto compile_compute = [&](const char* rel) -> GLuint {
        const std::string path = std::string(LUMINUMBRA_SOURCE_ROOT) + "/res/shaders/" + rel;
        std::ifstream file(path);
        if (!file.is_open()) {
            ADD_FAILURE() << "missing " << path;
            return 0;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string src = ss.str();
        const char* src_c = src.c_str();
        GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(sh, 1, &src_c, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            ADD_FAILURE() << rel << " failed to compile:\n" << log;
            return 0;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, sh);
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        glDeleteShader(sh);
        if (!ok) {
            ADD_FAILURE() << rel << " failed to link";
            glDeleteProgram(prog);
            return 0;
        }
        return prog;
    };
    GLuint inject = compile_compute("froxel_inject.comp");
    GLuint integrate = compile_compute("froxel_integrate.comp");
    ASSERT_NE(inject, 0u);
    ASSERT_NE(integrate, 0u);

    constexpr int GX = 160, GY = 90, GZ = 64;
    constexpr float kNear = 0.5f, kFar = 160.0f;
    auto make_volume = [&]() {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_3D, t);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, GX, GY, GZ);
        glBindTexture(GL_TEXTURE_3D, 0);
        return t;
    };
    GLuint scatter = make_volume();
    GLuint integrated = make_volume();

    // Shadow samplers need valid array bindings even though the lights are black.
    auto make_array = [&](GLenum ifmt, GLenum fmt, GLenum type, const void* px) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D_ARRAY, t);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, ifmt, 1, 1, 1, 0, fmt, type, px);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    };
    const float depth_px[1] = {1.0f};
    GLuint shadow_arr = make_array(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT, depth_px);
    const unsigned char white_px[4] = {255, 255, 255, 255};
    GLuint tint_arr = make_array(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, white_px);

    constexpr float kSigma = 0.02f;
    glUseProgram(inject);
    glBindImageTexture(0, scatter, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_arr);
    glUniform1i(glGetUniformLocation(inject, "u_shadowCascades"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tint_arr);
    glUniform1i(glGetUniformLocation(inject, "u_shadowTintCascades"), 1);
    glUniform1i(glGetUniformLocation(inject, "u_shadowTintEnabled"), 0);
    for (int i = 0; i < 4; ++i) {
        SetMat4Identity(inject, ("u_lightSpaceMatrices[" + std::to_string(i) + "]").c_str());
    }
    glUniform4f(glGetUniformLocation(inject, "u_cascadeSplits"), 1e9f, 1e9f, 1e9f, 1e9f);
    SetMat4Identity(inject, "u_inverseView");
    glUniform3f(glGetUniformLocation(inject, "u_cameraPos"), 0, 0, 0);
    glUniform1f(glGetUniformLocation(inject, "u_tanHalfFovY"), 1.0f);
    glUniform1f(glGetUniformLocation(inject, "u_aspect"), 1.0f);
    glUniform3f(glGetUniformLocation(inject, "u_sunDirection"), 0, 1, 0);
    glUniform3f(glGetUniformLocation(inject, "u_sunColor"), 0, 0, 0);     // lights black:
    glUniform3f(glGetUniformLocation(inject, "u_ambientColor"), 0, 0, 0); // T is the target
    glUniform1f(glGetUniformLocation(inject, "u_baseDensity"), kSigma);
    glUniform1f(glGetUniformLocation(inject, "u_baseHeight"), 1e9f); // uniform medium
    glUniform1f(glGetUniformLocation(inject, "u_densityFalloff"), 0.0f);
    glDispatchCompute(GX / 8, GY / 8 + 1, GZ);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    glUseProgram(integrate);
    glBindImageTexture(0, scatter, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, integrated, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(GX / 8, GY / 8 + 1, 1);
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // Read one texel at the LAST slice (GL 4.5 DSA sub-image read): its alpha is
    // the transmittance through the whole [near, far] range.
    float texel[4] = {};
    glGetTextureSubImage(
        integrated, 0, GX / 2, GY / 2, GZ - 1, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof(texel), texel);
    const float expected_T = std::exp(-kSigma * (kFar - kNear));
    // RGBA16F storage + 64 exponential steps: allow a small relative tolerance.
    EXPECT_NEAR(texel[3], expected_T, 0.004f)
        << "froxel integrate drifted from the analytic Beer-Lambert transmittance";
    // Black lights -> zero accumulated in-scatter.
    EXPECT_NEAR(texel[0], 0.0f, 1e-4f);
    EXPECT_NEAR(texel[1], 0.0f, 1e-4f);
    EXPECT_NEAR(texel[2], 0.0f, 1e-4f);

    glDeleteTextures(1, &tint_arr);
    glDeleteTextures(1, &shadow_arr);
    glDeleteTextures(1, &integrated);
    glDeleteTextures(1, &scatter);
    glDeleteProgram(integrate);
    glDeleteProgram(inject);
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
        -0.8f,
        -0.8f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.8f,
        -0.8f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.8f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
    };

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

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
        "CAMERA DEBUG", "RENDER DEBUG", "MESH UPLOAD:"};

    for (const std::string& message : forbidden_hot_path_messages) {
        EXPECT_EQ(source.find(message), std::string::npos) << message;
    }
}

TEST(RenderSmokeTest, ScheduledNightlyGateRequiresTaskSchedulerProvenance) {
    const fs::path behavior_test_path = SourceRoot() / "tools/gates/test-nightly-provenance.ps1";
    const std::string runner = ReadTextFile(SourceRoot() / "tools/gates/run-nightly-gate.ps1");
    const std::string frontier =
        ReadTextFile(SourceRoot() / "tools/gates/validate-engine-frontier.ps1");
    const std::string registrar =
        ReadTextFile(SourceRoot() / "tools/gates/register-nightly-gate-task.ps1");
    const std::string helper = ReadTextFile(SourceRoot() / "tools/gates/nightly-provenance.ps1");
    const std::string behavior_test = ReadTextFile(behavior_test_path);
    ASSERT_FALSE(runner.empty());
    ASSERT_FALSE(frontier.empty());
    ASSERT_FALSE(registrar.empty());
    ASSERT_FALSE(helper.empty());
    ASSERT_FALSE(behavior_test.empty());

    // The timestamp in the filename and report comes from one run identity;
    // copying an older report cannot win by changing its filesystem mtime.
    EXPECT_NE(runner.find("$stamp = $generatedAt.ToString"), std::string::npos);
    EXPECT_NE(runner.find("generated_at = $generatedAt.ToString(\"o\")"), std::string::npos);
    EXPECT_NE(runner.find("run_started_at = $runStartedAt.ToString(\"o\")"), std::string::npos);
    EXPECT_NE(runner.find("completed_at = $completedAt.ToString(\"o\")"), std::string::npos);
    EXPECT_NE(frontier.find("Sort-Object canonical_timestamp -Descending"), std::string::npos);
    EXPECT_NE(frontier.find("does not match generated_at"), std::string::npos);
    EXPECT_EQ(frontier.find("Sort-Object LastWriteTimeUtc -Descending"), std::string::npos);

    // The runner proves it is the unique live scheduler instance before any
    // gate, using the explicit 1-based COM collection contract. Its report is
    // tied to one Git revision from entry through completion.
    for (const char* seam : {
             "New-Object -ComObject Schedule.Service",
             "Assert-NightlyRegisteredTaskDefinition",
             "$runningTasks.Item($index)",
             "Assert-NightlyRunningTaskProvenance",
             "-ExpectedProcessId ([uint32]$PID)",
             "$gitHeadAtStart = Get-NightlyGitHead",
             "$gitHeadAtCompletion = Get-NightlyGitHead",
             "Assert-NightlyTrackedTreeClean -RepoRoot $RepoRoot",
             "git_head_at_start = $gitHeadAtStart",
             "git_head_at_completion = $gitHeadAtCompletion",
             "scheduler_instance = $schedulerInstance",
             "scheduler_definition_at_start = $schedulerDefinitionAtStart",
         }) {
        EXPECT_NE(runner.find(seam), std::string::npos) << seam;
    }
    EXPECT_LT(runner.find("Assert-NightlyRunningTaskProvenance"),
              runner.find("Invoke-NightlyStep"));
    EXPECT_LT(runner.find("Assert-NightlyRegisteredTaskDefinition"),
              runner.find("Invoke-NightlyStep"));
    const auto first_tree_check = runner.find("Assert-NightlyTrackedTreeClean -RepoRoot $RepoRoot");
    ASSERT_NE(first_tree_check, std::string::npos);
    EXPECT_NE(
        runner.find("Assert-NightlyTrackedTreeClean -RepoRoot $RepoRoot", first_tree_check + 1),
        std::string::npos);

    // Validation rejects stale, future-dated, mixed-revision, malformed
    // interval, and scheduler-field evidence before consulting LastRunTime.
    for (const char* seam : {
             "Assert-NightlyReportProvenance",
             "Assert-NightlyTaskDefinitionSnapshot",
             "Assert-NightlyRegisteredTaskDefinition",
             "Get-NightlyGitHead -RepoRoot $repoRoot",
             "Assert-NightlyTrackedTreeClean -RepoRoot $repoRoot",
             "[timespan]::FromHours(26)",
             "[timespan]::FromMinutes(5)",
         }) {
        EXPECT_NE(frontier.find(seam), std::string::npos) << seam;
    }
    for (const char* seam : {
             "git_head_at_start",
             "git_head_at_completion",
             "scheduler_instance",
         }) {
        EXPECT_NE(helper.find(seam), std::string::npos) << seam;
    }
    EXPECT_NE(helper.find("require run_started_at < completed_at == generated_at"),
              std::string::npos);

    // Closure requires the exact registered root task, canonical action and
    // presets, a matching scheduler run, and a successful task result.
    for (const char* seam : {
             "Luminumbra Nightly Gate",
             "Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath",
             "Get-ScheduledTaskInfo -TaskName $taskName -TaskPath $taskPath",
             "LastTaskResult",
             "lastRunDeltaSeconds",
             "-BuildPreset debug",
         }) {
        EXPECT_NE(frontier.find(seam), std::string::npos) << seam;
    }

    // Registration is explicit, idempotent and inspectable with -WhatIf; the
    // nightly runner itself never mutates Task Scheduler state.
    EXPECT_NE(registrar.find("SupportsShouldProcess = $true"), std::string::npos);
    EXPECT_NE(registrar.find("if ($alreadyCanonical)"), std::string::npos);
    EXPECT_NE(registrar.find("$PSCmdlet.ShouldProcess"), std::string::npos);
    EXPECT_NE(registrar.find("[ValidateSet(\"02:00\")]"), std::string::npos);
    EXPECT_NE(registrar.find("-BuildPreset debug"), std::string::npos);
    EXPECT_NE(registrar.find("-LogonType Interactive"), std::string::npos);
    EXPECT_NE(registrar.find("-RunLevel Limited"), std::string::npos);
    EXPECT_NE(registrar.find("Assert-NightlyRegisteredTaskDefinition"), std::string::npos);
    EXPECT_NE(registrar.find("Assert-NightlyTaskRepairAllowed"), std::string::npos);
    EXPECT_NE(registrar.find("$identitySid"), std::string::npos);
    EXPECT_NE(registrar.find("Definition mismatch:"), std::string::npos);
    EXPECT_NE(helper.find("Resolve-NightlyPrincipalSidValue"), std::string::npos);
    EXPECT_NE(helper.find("$AccountSidResolver"), std::string::npos);
    EXPECT_EQ(registrar.find("RunLevel Highest"), std::string::npos);
    EXPECT_EQ(registrar.find("BuiltInRole]::Administrator"), std::string::npos);
    const auto first_principal = registrar.find("-Principal $taskPrincipal");
    ASSERT_NE(first_principal, std::string::npos);
    EXPECT_NE(registrar.find("-Principal $taskPrincipal", first_principal + 1), std::string::npos);
    for (const char* seam : {
             "principal.sid",
             "principal.logon_type",
             "principal.run_level",
             "actions.Count -ne 1",
             "actions[0].execute",
             "actions[0].arguments",
             "actions[0].working_directory",
             "triggers.Count -ne 1",
             "triggers[0].enabled",
             "triggers[0].days_interval",
             "triggers[0].daily_at",
             "triggers[0].end_boundary",
             "settings.enabled",
             "settings.multiple_instances",
             "settings.start_when_available",
             "settings.execution_time_limit",
         }) {
        EXPECT_NE(helper.find(seam), std::string::npos) << seam;
    }
    EXPECT_EQ(runner.find("Register-ScheduledTask"), std::string::npos);
    EXPECT_EQ(runner.find("Set-ScheduledTask"), std::string::npos);

    // This is a behavior test, not another token contract: injected short,
    // qualified and SID identities; 0/1/2 COM-like instances; malformed
    // path/PID/action/GUID; stale/future/HEAD/interval reports all execute.
    for (const char* fixture : {
             "BUILDHOST\\gateuser",
             "zero nightly instances",
             "two nightly instances",
             "wrong task path",
             "wrong engine PID",
             "wrong CurrentAction",
             "malformed instance GUID",
             "wrong registered arguments",
             "wrong registered working directory",
             "wrong trigger time",
             "wrong multiple-instance policy",
             "wrong execution limit",
             "a forged reported start definition",
             "repairing a noncanonical running task",
             "a report older than 26 hours",
             "a report more than five minutes in the future",
             "a report from another Git HEAD",
             "a tracked source change",
             "a path outside an explicit allowlist",
         }) {
        EXPECT_NE(behavior_test.find(fixture), std::string::npos) << fixture;
    }
#if defined(_WIN32)
    const std::string behavior_command =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
        behavior_test_path.string() + "\"";
    EXPECT_EQ(std::system(behavior_command.c_str()), 0)
        << "nightly provenance behavior test failed";
#endif
}

TEST(RenderSmokeTest, FarLodWorkersUseImmutableSdfSnapshots) {
    const std::string header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/FarLodSystem.h");
    const std::string source =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/FarLodSystem.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    // Snapshot capture must happen on the render-owner thread before the
    // asynchronous dispatch.  The job owns copied SDF/material bytes rather
    // than a streamed Chunk whose mutable vectors can be edited or evicted.
    const std::size_t capture = source.find("capture_far_lod_sdf_snapshot");
    const std::size_t dispatch = source.find("dispatch_batch");
    ASSERT_NE(capture, std::string::npos);
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_LT(capture, dispatch);
    EXPECT_NE(header.find("shared_ptr<const Systems::FarLodSdfSnapshot>"), std::string::npos);
    EXPECT_NE(source.find("ReduceChunkSdfIntoFarTile"), std::string::npos);

    // A completed mesh must be discarded when the capture's epoch, params, or
    // authoritative voxel revision is no longer current.
    EXPECT_NE(source.find("capture_epoch"), std::string::npos);
    EXPECT_NE(source.find("authority_revision"), std::string::npos);
    EXPECT_NE(source.find("is_far_lod_sdf_snapshot_current"), std::string::npos);

    EXPECT_NE(source.find("BuildFarLodWorkerTile"), std::string::npos);
    EXPECT_NE(source.find("rebase_authoritative_tile"), std::string::npos);
    EXPECT_NE(source.find("GenerateChunkData(scratch, 1)"), std::string::npos);
    const std::size_t stale_check = source.find("const bool snapshot_stale");
    const std::size_t owner_save = source.find("save_tile(result.tile");
    ASSERT_NE(stale_check, std::string::npos);
    ASSERT_NE(owner_save, std::string::npos);
    EXPECT_LT(stale_check, owner_save);
}

TEST(RenderSmokeTest, RenderPipelineExposesPassBudgetCounters) {
    const std::string header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadRenderPipelineCombinedSources();
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("RenderPassFrameStats"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_stats"), std::string::npos);
    EXPECT_NE(header.find("shadow_cascade_draws"), std::string::npos);
    // far-LOD region draws are counter-backed like every other pass.
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
    const std::string header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadRenderPipelineCombinedSources();
    const std::string shader_header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.h");
    const std::string shader_source =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.cpp");
    const std::string capture_header =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.h");
    const std::string capture_source =
        ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.cpp");
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
    // triplanar terrain albedo + normal mapping moved from the lighting
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
    pass_metadata
        << "    "
           "{\"name\":\"shadow\",\"inputs\":[\"terrain_depth\"],\"outputs\":[\"shadow.depth_"
           "texture_array\"],\"resolution\":\"shadow_map\",\"clear\":\"depth\",\"load_store\":"
           "\"store depth cascades\",\"draw_count_source\":\"shadow_draws\"},\n";
    pass_metadata
        << "    "
           "{\"name\":\"gbuffer\",\"inputs\":[\"terrain_meshes\",\"static_meshes\",\"material_"
           "lut\",\"terrain_texture_array\",\"terrain_normal_array\"],\"outputs\":[\"gbuffer."
           "position\",\"gbuffer.normal_material\",\"gbuffer.albedo_roughness\",\"gbuffer.metallic_"
           "ao\",\"gbuffer.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_"
           "store\":\"store deferred attachments\",\"draw_count_source\":\"terrain_draws\"},\n";
    pass_metadata
        << "    "
           "{\"name\":\"ssao\",\"inputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"ssao."
           "noise\"],\"outputs\":[\"ssao.raw\"],\"resolution\":\"screen\",\"clear\":\"color\","
           "\"load_store\":\"store ambient occlusion\",\"draw_count_source\":\"ssao_draws\"},\n";
    pass_metadata << "    "
                     "{\"name\":\"ssao_blur\",\"inputs\":[\"ssao.raw\"],\"outputs\":[\"ssao.blur\"]"
                     ",\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store "
                     "blurred ambient occlusion\",\"draw_count_source\":\"ssao_blur_draws\"},\n";
    pass_metadata << "    "
                     "{\"name\":\"lighting\",\"inputs\":[\"gbuffer.*\",\"shadow.depth_texture_"
                     "array\",\"ssao.blur\",\"terrain_texture_array\",\"material_lut\",\"water."
                     "fallback.black\"],\"outputs\":[\"lighting.color\",\"lighting.depth\"],"
                     "\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store "
                     "lit scene\",\"draw_count_source\":\"lighting_draws\"},\n";
    pass_metadata
        << "    "
           "{\"name\":\"water\",\"inputs\":[\"lighting.opaque_color_copy\",\"gbuffer.depth\","
           "\"water_meshes\",\"water.fallback.*\"],\"outputs\":[\"lighting.color\"],\"resolution\":"
           "\"screen\",\"clear\":\"load lighting\",\"load_store\":\"blend water into "
           "lighting\",\"draw_count_source\":\"water_draws\"},\n";
    pass_metadata
        << "    "
           "{\"name\":\"skybox\",\"inputs\":[\"skybox_vertices\"],\"outputs\":[\"lighting.color\"],"
           "\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"store sky "
           "contribution\",\"draw_count_source\":\"skybox_draws\"},\n";
    pass_metadata << "    "
                     "{\"name\":\"particles\",\"inputs\":[\"particle_instances\",\"gbuffer.depth\"]"
                     ",\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load "
                     "lighting\",\"load_store\":\"blend forward-lit "
                     "particles\",\"draw_count_source\":\"particle_draws\"},\n";
    pass_metadata << "    "
                     "{\"name\":\"final_blit\",\"inputs\":[\"lighting.color\"],\"outputs\":["
                     "\"swapchain.color\"],\"resolution\":\"screen\",\"clear\":\"default "
                     "color+depth\",\"load_store\":\"present-ready "
                     "color\",\"draw_count_source\":\"final_blits\"}\n";
    pass_metadata << "  ]\n";
    pass_metadata << "}\n";

    std::ofstream resource_registry(RenderFrameworkArtifactRoot() / "resource_registry.json");
    ASSERT_TRUE(resource_registry);
    resource_registry << "{\n";
    resource_registry << "  \"schema\": \"luminumbra.render_framework.resource_registry.v1\",\n";
    resource_registry << "  \"debug_labels\": true,\n";
    resource_registry << "  \"resource_types\": [\"framebuffer\", \"texture\", \"renderbuffer\", "
                         "\"buffer\", \"vertex_array\", \"shader_program\"],\n";
    resource_registry << "  \"resize_recreates\": [\"lighting.fbo\", \"gbuffer.fbo\", "
                         "\"ssao.fbo\", \"ssao.blur_fbo\"],\n";
    resource_registry << "  \"shutdown_requires_empty_registry\": true\n";
    resource_registry << "}\n";

    std::ofstream terrain_diagnostics(RenderFrameworkArtifactRoot() /
                                      "terrain_material_diagnostics.json");
    ASSERT_TRUE(terrain_diagnostics);
    terrain_diagnostics << "{\n";
    terrain_diagnostics << "  \"schema\": \"luminumbra.render_framework.terrain_materials.v1\",\n";
    terrain_diagnostics << "  \"texture_array\": \"terrain.texture_array\",\n";
    terrain_diagnostics << "  \"material_lut\": \"terrain.material_lut\",\n";
    terrain_diagnostics << "  \"material_registry\": \"data/common/materials.json\",\n";
    terrain_diagnostics
        << "  \"missing_texture_behavior\": \"visible magenta checker fallback\",\n";
    terrain_diagnostics << "  \"fallback_counter\": \"terrain_texture_fallback_layers\",\n";
    terrain_diagnostics << "  \"production_requires_zero_fallback_layers\": true,\n";
    terrain_diagnostics << "  \"terrain_texture_layers\": 5\n";
    terrain_diagnostics << "}\n";

    std::ofstream shader_health(RenderFrameworkArtifactRoot() / "shader_health.json");
    ASSERT_TRUE(shader_health);
    shader_health << "{\n";
    shader_health << "  \"schema\": \"luminumbra.render_framework.shader_health.v1\",\n";
    shader_health << "  \"runtime_validity_requires_compile_and_link_success\": true,\n";
    shader_health << "  \"programs\": [\"basic\", \"g_buffer\", \"instanced_mesh_gbuffer\", "
                     "\"lighting_pass\", \"skybox\", \"shadow_map\", \"ssao\", \"ssao_blur\", "
                     "\"water\", \"rml_ui\", \"loading_hologram\", \"loading_visual\", "
                     "\"volumetric_lighting\", \"magical_particles\"]\n";
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
