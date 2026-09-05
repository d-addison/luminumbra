// shader-resource reflection + layout-validation unit tests.
//
// Compiles a small known program in a hidden GL 4.5 context, introspects its
// resource layout, and asserts:
//   * samplers reflect with the right GL type and post-link unit
//     (including a layout(binding=) sampler and a UBO block binding);
//   * a CORRECT pass expectation validates clean;
//   * a TYPE mismatch and a wrong-unit / wrong-binding expectation HARD-FAIL
//     (ok=false) -- the load-time tripwire and the hot-reload ROLLBACK decision;
//   * an ABSENT expected sampler is a soft warning, not a failure (the GL linker
//     strips unused uniforms).
//
// GTEST_SKIPs on a headless machine without a usable GL context (matches the
// other GL render tests' policy).

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rendering/PassShaderLayouts.h" // /: the enumerable layout registry
#include "rendering/ShaderReflection.h"

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
        m_window = glfwCreateWindow(64, 64, "shader_reflection_test", nullptr, nullptr);
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

bool ReadShaderSourceRecursive(const std::filesystem::path& path,
                               std::unordered_set<std::string>& include_stack,
                               std::string& out,
                               std::string& err) {
    const std::filesystem::path normalized = std::filesystem::absolute(path).lexically_normal();
    const std::string key = normalized.generic_string();
    if (!include_stack.insert(key).second) {
        err = "cyclic shader include: " + key;
        return false;
    }

    std::ifstream f(normalized);
    if (!f) {
        err = "cannot open " + key;
        include_stack.erase(key);
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        constexpr std::string_view prefix = "#include \"";
        const std::size_t begin = line.find(prefix);
        if (begin == std::string::npos) {
            out.append(line).push_back('\n');
            continue;
        }

        const std::size_t name_begin = begin + prefix.size();
        const std::size_t name_end = line.find('"', name_begin);
        if (begin != 0 || name_end == std::string::npos || name_end + 1 != line.size()) {
            err = "malformed shader include in " + key + ": " + line;
            include_stack.erase(key);
            return false;
        }

        const std::filesystem::path included =
            normalized.parent_path() / line.substr(name_begin, name_end - name_begin);
        if (!ReadShaderSourceRecursive(included, include_stack, out, err)) {
            include_stack.erase(key);
            return false;
        }
    }

    include_stack.erase(key);
    return true;
}

bool ReadShaderSource(const std::string& path, std::string& out, std::string& err) {
    std::unordered_set<std::string> include_stack;
    return ReadShaderSourceRecursive(path, include_stack, out, err);
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

constexpr const char* kVs = R"(#version 450 core
layout(location = 0) in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); }
)";

// u_color carries an explicit layout(binding=3); u_layers is a 2D ARRAY sampler;
// Params is a std140 UBO at binding 2. All three are used so none are stripped.
constexpr const char* kFs = R"(#version 450 core
out vec4 FragColor;
layout(binding = 3) uniform sampler2D u_color;
uniform sampler2DArray u_layers;
layout(std140, binding = 2) uniform Params { vec4 tint; } u_params;
void main() {
    FragColor = texture(u_color, vec2(0.5))
              + texture(u_layers, vec3(0.5))
              + u_params.tint;
}
)";

struct ReflectionFixture : public ::testing::Test {
    HiddenGlContext ctx;
    GLuint program = 0;
    ReflectedLayout layout;

    void SetUp() override {
        if (!ctx.ready())
            GTEST_SKIP() << "no GL context: " << ctx.error();
        std::string err;
        program = LinkProgram(kVs, kFs, err);
        ASSERT_NE(program, 0u) << "test program failed to build: " << err;
        layout = ReflectProgramLayout(program);
    }
    void TearDown() override {
        if (program)
            glDeleteProgram(program);
    }
};

TEST_F(ReflectionFixture, ReflectsSamplersWithTypeAndUnit) {
    const ReflectedSampler* color = layout.find_sampler("u_color");
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->type, static_cast<GLenum>(GL_SAMPLER_2D));
    EXPECT_EQ(color->unit, 3) << "layout(binding=3) default unit should reflect";

    const ReflectedSampler* layers = layout.find_sampler("u_layers");
    ASSERT_NE(layers, nullptr);
    EXPECT_EQ(layers->type, static_cast<GLenum>(GL_SAMPLER_2D_ARRAY));
    EXPECT_EQ(layers->unit, 0) << "no binding qualifier -> default unit 0";
}

TEST_F(ReflectionFixture, ReflectsUniformBlockBinding) {
    const ReflectedBlock* params = layout.find_uniform_block("Params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->binding, 2);
}

TEST_F(ReflectionFixture, ReflectsFragmentOutput) {
    ASSERT_FALSE(layout.outputs.empty());
    bool found = false;
    for (const auto& o : layout.outputs) {
        if (o.name == "FragColor") {
            found = true;
            EXPECT_GE(o.location, 0);
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ReflectionFixture, CorrectExpectationValidatesClean) {
    ExpectedLayout exp;
    exp.pass_name = "test";
    exp.samplers = {
        {"u_color", GL_SAMPLER_2D, 3}, // unit check exercised (binding=3)
        {"u_layers", GL_SAMPLER_2D_ARRAY, -1},
    };
    exp.uniform_blocks = {{"Params", 2}};
    const ValidationResult vr = ValidateReflectedLayout(layout, exp);
    EXPECT_TRUE(vr.ok) << vr.diagnostic;
    EXPECT_FALSE(vr.had_warning) << vr.diagnostic;
}

TEST_F(ReflectionFixture, TypeMismatchHardFails) {
    // Pass adopts u_color as a 2D ARRAY but the shader declares plain sampler2D --
    // exactly the "renders garbage" bug  catches at load.
    ExpectedLayout exp;
    exp.pass_name = "test";
    exp.samplers = {{"u_color", GL_SAMPLER_2D_ARRAY, -1}};
    const ValidationResult vr = ValidateReflectedLayout(layout, exp);
    EXPECT_FALSE(vr.ok);
    EXPECT_NE(vr.diagnostic.find("TYPE mismatch"), std::string::npos);
}

TEST_F(ReflectionFixture, WrongUnitHardFails) {
    ExpectedLayout exp;
    exp.pass_name = "test";
    exp.samplers = {{"u_color", GL_SAMPLER_2D, 5}}; // shader says binding=3
    const ValidationResult vr = ValidateReflectedLayout(layout, exp);
    EXPECT_FALSE(vr.ok);
    EXPECT_NE(vr.diagnostic.find("UNIT mismatch"), std::string::npos);
}

TEST_F(ReflectionFixture, WrongBlockBindingHardFails) {
    ExpectedLayout exp;
    exp.pass_name = "test";
    exp.uniform_blocks = {{"Params", 7}}; // shader says binding=2
    const ValidationResult vr = ValidateReflectedLayout(layout, exp);
    EXPECT_FALSE(vr.ok);
    EXPECT_NE(vr.diagnostic.find("BINDING mismatch"), std::string::npos);
}

TEST_F(ReflectionFixture, AbsentSamplerIsSoftWarningNotFailure) {
    // An expected-but-unused sampler is stripped by the linker -> warn, don't fail.
    ExpectedLayout exp;
    exp.pass_name = "test";
    exp.samplers = {{"u_does_not_exist", GL_SAMPLER_2D, -1}};
    const ValidationResult vr = ValidateReflectedLayout(layout, exp);
    EXPECT_TRUE(vr.ok) << "absence alone must not hard-fail";
    EXPECT_TRUE(vr.had_warning);
}

// The Reload ROLLBACK decision is exactly "candidate layout fails validation ->
// do not adopt". Prove that decision predicate here at the free-function level
// (decoupled from file IO): a good candidate validates, a bad candidate does not.
TEST_F(ReflectionFixture, RollbackPredicateRejectsBadCandidateLayout) {
    ExpectedLayout registered; // what the pass declared on first good load
    registered.pass_name = "test";
    registered.samplers = {{"u_color", GL_SAMPLER_2D, -1}};

    // A recompiled program whose u_color became a 2D array fails the registered
    // expectation -> Reload keeps the previous program.
    std::string err;
    const char* bad_fs = R"(#version 450 core
out vec4 FragColor;
uniform sampler2DArray u_color;
void main() { FragColor = texture(u_color, vec3(0.5)); }
)";
    GLuint bad = LinkProgram(kVs, bad_fs, err);
    ASSERT_NE(bad, 0u) << err;
    const ReflectedLayout bad_layout = ReflectProgramLayout(bad);
    EXPECT_FALSE(ValidateReflectedLayout(bad_layout, registered).ok);
    // And the original good program still satisfies it.
    EXPECT_TRUE(ValidateReflectedLayout(layout, registered).ok);
    glDeleteProgram(bad);
}

// ----------------------------------------------------------------------------
// the startup layout-validation gate as per-pass fixtures. For every
// entry in the PassShaderLayouts registry, load the REAL shader pair, reflect the
// linked program, and assert its registered ExpectedLayout validates:
//   * ok (no TYPE / UNIT / BINDING mismatch -- the "renders garbage" tripwire), AND
//   * NON-vacuously: no expected sampler was stripped/absent (had_warning), and every
//     declared sampler actually reflects. This is what stops a layout from "passing"
//     by declaring samplers the shader never samples.
// GTEST_SKIPs headless (needs a real GL context to compile the shaders).
// ----------------------------------------------------------------------------

std::vector<std::string> RegisteredLayoutNames() {
    std::vector<std::string> names;
    for (const auto& e : AllPassShaderLayouts())
        names.push_back(e.layout_name);
    return names;
}

class PassLayoutValidation : public ::testing::TestWithParam<std::string> {
protected:
    HiddenGlContext ctx;
    void SetUp() override {
        if (!ctx.ready())
            GTEST_SKIP() << "no GL context: " << ctx.error();
    }
};

TEST_P(PassLayoutValidation, RealShaderMatchesRegisteredLayoutNonVacuously) {
    const PassShaderLayout* entry = FindPassShaderLayout(GetParam());
    ASSERT_NE(entry, nullptr) << "registry lookup failed for " << GetParam();

    const std::string root = LUMINUMBRA_SOURCE_ROOT;
    std::string vsrc, fsrc, ioerr;
    ASSERT_TRUE(ReadShaderSource(root + "/" + entry->vert, vsrc, ioerr)) << ioerr;
    ASSERT_TRUE(ReadShaderSource(root + "/" + entry->frag, fsrc, ioerr)) << ioerr;

    std::string linkerr;
    GLuint prog = LinkProgram(vsrc.c_str(), fsrc.c_str(), linkerr);
    ASSERT_NE(prog, 0u) << entry->layout_name << " (" << entry->frag
                        << ") failed to build: " << linkerr;

    const ReflectedLayout reflected = ReflectProgramLayout(prog);
    const ValidationResult vr = ValidateReflectedLayout(reflected, entry->expected);

    EXPECT_TRUE(vr.ok) << entry->layout_name << ": " << vr.diagnostic;
    EXPECT_FALSE(vr.had_warning)
        << entry->layout_name << ": an expected sampler is not sampled by the shader (stripped) -> "
        << vr.diagnostic;
    for (const auto& s : entry->expected.samplers) {
        EXPECT_NE(reflected.find_sampler(s.name), nullptr)
            << entry->layout_name << " expects sampler '" << s.name
            << "' but the linked shader does not sample it";
    }
    glDeleteProgram(prog);
}

INSTANTIATE_TEST_SUITE_P(RegisteredPasses,
                         PassLayoutValidation,
                         ::testing::ValuesIn(RegisteredLayoutNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });

// ----------------------------------------------------------------------------
// the ReflectionCoverage gate. Source-scan every pass under
// rendering/passes/: a pass that binds samplers (glActiveTexture) MUST also
// register + validate a layout (Shader::ValidateLayout), unless it is explicitly
// exempted with a reason. Fails listing the unvalidated passes. Pure source scan
// (no GL) -- runs even headless. This is the complement of the fixtures above:
// they prove every REGISTERED layout is real; this proves nothing BINDS samplers
// without being registered.
// ----------------------------------------------------------------------------
TEST(ReflectionCoverage, EverySamplerBindingPassRegistersAndValidatesALayout) {
    namespace fs = std::filesystem;
    const fs::path passes_dir =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "src/luminumbra_client/rendering/passes";
    ASSERT_TRUE(fs::exists(passes_dir)) << "passes dir not found: " << passes_dir.string();

    // filename -> why it cannot register a layout. An exemption is not a waiver:
    // the staleness check below fails if an exempt pass stops binding samplers,
    // so an entry cannot quietly outlive its reason.
    const std::map<std::string, std::string> kExempt{
        {"LuminanceMeterPass.cpp",
         "drives a COMPUTE shader (res/shaders/luminance_reduce.comp) built with raw "
         "glCreateShader(GL_COMPUTE_SHADER), not the Shader class. Shader is "
         "vertex+fragment only (Shader.h:14) and PassShaderLayout requires a vert/frag "
         "pair, so there is no Shader to call ValidateLayout on and no entry this "
         "registry can express. Covering compute programs means teaching Shader and "
         "PassShaderLayouts about them, which is a change to the validation machinery "
         "rather than to this pass."},
    };

    std::vector<std::string> unvalidated;  // binds samplers but never validates
    std::vector<std::string> stale_exempt; // exempt but no longer binds samplers

    for (const auto& de : fs::directory_iterator(passes_dir)) {
        if (!de.is_regular_file() || de.path().extension() != ".cpp")
            continue;
        const std::string name = de.path().filename().string();
        std::string src, ioerr;
        ASSERT_TRUE(ReadTextFile(de.path().string(), src, ioerr)) << ioerr;

        const bool binds_samplers = src.find("glActiveTexture") != std::string::npos;
        const bool validates = src.find("ValidateLayout") != std::string::npos;

        if (kExempt.count(name)) {
            if (!binds_samplers)
                stale_exempt.push_back(name);
            continue;
        }
        if (binds_samplers && !validates)
            unvalidated.push_back(name);
    }

    std::string unvalidated_msg;
    for (const auto& n : unvalidated)
        unvalidated_msg += "\n  - " + n;
    EXPECT_TRUE(unvalidated.empty())
        << "these passes bind samplers (glActiveTexture) but never call "
           "Shader::ValidateLayout -- declare their layout in PassShaderLayouts "
           "and validate it in init_shader(), or add a reasoned exemption:"
        << unvalidated_msg;

    std::string stale_msg;
    for (const auto& n : stale_exempt)
        stale_msg += "\n  - " + n;
    EXPECT_TRUE(stale_exempt.empty())
        << "these passes are exempted from the layout gate but no longer bind "
           "samplers -- drop the stale exemption:"
        << stale_msg;
}

} // namespace
