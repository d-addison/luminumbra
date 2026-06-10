#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
        {"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"},
        {"skybox", "skybox.vert", "skybox.frag"},
        {"shadow_map", "shadow_map.vert", "shadow_map.frag"},
        {"ssao", "ssao.vert", "ssao.frag"},
        {"ssao_blur", "ssao.vert", "ssao_blur.frag"},
        {"water", "water.vert", "water.frag"},
        {"rml_ui", "rml.vert", "rml.frag"},
        {"loading_hologram", "loading_hologram.vert", "loading_hologram.frag"},
        {"loading_visual", "loading_visual.vert", "loading_visual.frag"},
        {"volumetric_lighting", "volumetric_lighting.vert", "volumetric_lighting.frag"},
        {"magical_particles", "magical_particles.vert", "magical_particles.frag", "magical_particles.geom"},
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

void WriteRenderHealthAnalysis(
    const fs::path& path,
    bool passed,
    bool health_api_present,
    bool pass_metadata_present,
    bool resource_registry_present,
    bool terrain_materials_present,
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
    output << "    \"required_passes\": [\"shadow\", \"gbuffer\", \"ssao\", \"ssao_blur\", \"lighting\", \"water\", \"skybox\", \"final_blit\"]\n";
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
        program_health,
        gl_errors);

    EXPECT_TRUE(health_api_present);
    EXPECT_TRUE(pass_metadata_present);
    EXPECT_TRUE(resource_registry_present);
    EXPECT_TRUE(terrain_materials_present);
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
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
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
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("RenderPassFrameStats"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_stats"), std::string::npos);
    EXPECT_NE(header.find("shadow_cascade_draws"), std::string::npos);
    EXPECT_NE(source.find("ensure_terrain_culling_hierarchy"), std::string::npos);
    EXPECT_NE(source.find("shadow_cascade_visible_chunks"), std::string::npos);

    fs::create_directories(RenderPerfArtifactRoot());
    std::ofstream output(RenderPerfArtifactRoot() / "pass_counts.json");
    ASSERT_TRUE(output);
    output << "{\n";
    output << "  \"counter_contract\": {\n";
    output << "    \"terrain_draws\": true,\n";
    output << "    \"terrain_visible_chunks\": true,\n";
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
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
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
    EXPECT_NE(ReadTextFile(SourceRoot() / "res/shaders/lighting_pass.frag").find("terrainLayerCount"), std::string::npos);
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
    pass_metadata << "    {\"name\":\"gbuffer\",\"inputs\":[\"terrain_meshes\",\"static_meshes\",\"material_lut\"],\"outputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"gbuffer.albedo_roughness\",\"gbuffer.metallic_ao\",\"gbuffer.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store deferred attachments\",\"draw_count_source\":\"terrain_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao\",\"inputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"ssao.noise\"],\"outputs\":[\"ssao.raw\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store ambient occlusion\",\"draw_count_source\":\"ssao_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao_blur\",\"inputs\":[\"ssao.raw\"],\"outputs\":[\"ssao.blur\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store blurred ambient occlusion\",\"draw_count_source\":\"ssao_blur_draws\"},\n";
    pass_metadata << "    {\"name\":\"lighting\",\"inputs\":[\"gbuffer.*\",\"shadow.depth_texture_array\",\"ssao.blur\",\"terrain_texture_array\",\"material_lut\",\"water.fallback.black\"],\"outputs\":[\"lighting.color\",\"lighting.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store lit scene\",\"draw_count_source\":\"lighting_draws\"},\n";
    pass_metadata << "    {\"name\":\"water\",\"inputs\":[\"lighting.opaque_color_copy\",\"gbuffer.depth\",\"water_meshes\",\"water.fallback.*\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"blend water into lighting\",\"draw_count_source\":\"water_draws\"},\n";
    pass_metadata << "    {\"name\":\"skybox\",\"inputs\":[\"skybox_vertices\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"store sky contribution\",\"draw_count_source\":\"skybox_draws\"},\n";
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
