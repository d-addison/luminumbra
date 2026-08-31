//   /   +: PilotShaderReflectionParity.
//
// For each pilot shader (ssao, debug_view), assert that THREE independent views of
// its sampler interface agree on {name -> GL sampler type}:
//   A = slangc -reflection-json of the single-source HLSL (build artifact)
//   B = GL program introspection of the shipping GLSL (ReflectProgramLayout)
//   C = the declared ExpectedLayout (PassShaderLayouts, the validation currency)
// A == C is GL-free and always runs (non-vacuous coverage even headless); B == C and
// A == B add the GL-introspection leg when a context is available. This proves the
// HLSL port reflects the SAME interface the engine already validates -- the
// reflection half of ; the ported-HLSL render + FLIP-vs-golden lands with
// the pilot.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "rendering/PassShaderLayouts.h"
#include "rendering/ShaderReflection.h"
#include "rendering/ShaderReflectionSpirv.h"

using namespace Luminumbra::Rendering;

namespace {

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
        m_window = glfwCreateWindow(64, 64, "pilot_shader_reflection_test", nullptr, nullptr);
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
        if (m_window)
            glfwDestroyWindow(m_window);
        if (m_glfw_initialized)
            glfwTerminate();
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

GLuint CompileStage(GLenum stage, const char* src, std::string& err) {
    GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        err = log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint LinkProgram(const char* vs, const char* fs, std::string& err) {
    GLuint v = CompileStage(GL_VERTEX_SHADER, vs, err);
    if (!v)
        return 0;
    GLuint f = CompileStage(GL_FRAGMENT_SHADER, fs, err);
    if (!f) {
        glDeleteShader(v);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint linked = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        err = log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool ReadTextFile(const std::string& path, std::string& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::map<std::string, GLenum> ReflectedSamplerMap(const std::vector<ReflectedSampler>& s) {
    std::map<std::string, GLenum> m;
    for (const auto& x : s)
        m[x.name] = x.type;
    return m;
}
std::map<std::string, GLenum> ExpectedSamplerMap(const std::vector<ExpectedSampler>& s) {
    std::map<std::string, GLenum> m;
    for (const auto& x : s)
        m[x.name] = x.type;
    return m;
}

std::string SourceRoot() {
    return LUMINUMBRA_SOURCE_ROOT;
}
std::string ReflectDir() {
    return PILOT_SHADER_REFLECT_DIR;
}

class PilotShaderReflectionParityGpu : public ::testing::Test {
protected:
    static HiddenGlContext* s_ctx;
    static void SetUpTestSuite() {
        s_ctx = new HiddenGlContext();
    }
    static void TearDownTestSuite() {
        delete s_ctx;
        s_ctx = nullptr;
    }
};
HiddenGlContext* PilotShaderReflectionParityGpu::s_ctx = nullptr;

void RunParity(HiddenGlContext* ctx, const char* pass_name, const char* json_basename) {
    // C: the declared ExpectedLayout (validation currency).
    const PassShaderLayout* entry = FindPassShaderLayout(pass_name);
    ASSERT_NE(entry, nullptr) << "no PassShaderLayout registered for '" << pass_name << "'";
    const std::map<std::string, GLenum> expected = ExpectedSamplerMap(entry->expected.samplers);
    ASSERT_FALSE(expected.empty()) << "'" << pass_name << "' declares no samplers";

    // A: Slang reflection of the slangc-compiled HLSL (GL-free).
    const std::string json_path = ReflectDir() + "/" + json_basename;
    std::string json_text, err;
    ASSERT_TRUE(ReadTextFile(json_path, json_text, err)) << err;
    const ReflectedLayout slang = ReflectSlangReflectionJson(json_text);
    const std::map<std::string, GLenum> slang_map = ReflectedSamplerMap(slang.samplers);
    ASSERT_FALSE(slang_map.empty())
        << "Slang reflection of " << json_basename << " produced no samplers (vacuous)";

    // A == C: the HLSL's reflected interface equals the declared layout.
    EXPECT_EQ(slang_map, expected)
        << "Slang reflected layout != declared ExpectedLayout for '" << pass_name << "'";

    // B: GL introspection of the shipping GLSL (adds the third source when GL is up).
    if (ctx != nullptr && ctx->ready()) {
        std::string vs, fs;
        ASSERT_TRUE(ReadTextFile(SourceRoot() + "/" + entry->vert, vs, err)) << err;
        ASSERT_TRUE(ReadTextFile(SourceRoot() + "/" + entry->frag, fs, err)) << err;
        std::string link_err;
        const GLuint prog = LinkProgram(vs.c_str(), fs.c_str(), link_err);
        ASSERT_NE(prog, 0u) << "GLSL link failed for '" << pass_name << "': " << link_err;
        const ReflectedLayout gl = ReflectProgramLayout(prog);
        glDeleteProgram(prog);
        const std::map<std::string, GLenum> gl_map = ReflectedSamplerMap(gl.samplers);

        EXPECT_EQ(gl_map, expected)
            << "GL-introspected layout != declared ExpectedLayout for '" << pass_name << "'";
        EXPECT_EQ(slang_map, gl_map)
            << "Slang layout != GL-introspected layout for '" << pass_name << "'";
    } else {
        // A==C already gave non-vacuous coverage; the GL leg is unavailable headless.
        GTEST_LOG_(WARNING) << "no GL context; GL-introspection leg skipped for '" << pass_name
                            << "' (A==C still asserted)";
    }
}

TEST_F(PilotShaderReflectionParityGpu, SsaoLayoutMatchesAcrossReflectionSources) {
    RunParity(s_ctx, "ssao", "ssao.frag.reflect.json");
}

TEST_F(PilotShaderReflectionParityGpu, DebugViewLayoutMatchesAcrossReflectionSources) {
    RunParity(s_ctx, "debug_view", "debug_view.frag.reflect.json");
}

} // namespace
