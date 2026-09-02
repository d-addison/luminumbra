#include "Shader.h"
#include "core/Log.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Luminumbra::Rendering {

namespace {

bool LoadShaderSourceRecursive(const std::filesystem::path& path,
                               std::unordered_set<std::string>& include_stack,
                               std::string& output,
                               std::string& diagnostic) {
    const std::filesystem::path normalized = std::filesystem::absolute(path).lexically_normal();
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
            output.append(line).push_back('\n');
            continue;
        }
        const std::size_t name_begin = begin + prefix.size();
        const std::size_t name_end = line.find('"', name_begin);
        if (begin != 0 || name_end == std::string::npos || name_end + 1 != line.size()) {
            diagnostic = "malformed shader include in " + key + ": " + line;
            include_stack.erase(key);
            return false;
        }
        const std::filesystem::path included =
            normalized.parent_path() / line.substr(name_begin, name_end - name_begin);
        if (!LoadShaderSourceRecursive(included, include_stack, output, diagnostic)) {
            include_stack.erase(key);
            return false;
        }
    }
    include_stack.erase(key);
    return true;
}

bool LoadShaderSource(const char* path, std::string& output, std::string& diagnostic) {
    std::unordered_set<std::string> include_stack;
    return LoadShaderSourceRecursive(path, include_stack, output, diagnostic);
}

} // namespace

// Compile+link a fresh program from the two source files. Returns a NEW program
// name, or 0 on any IO/compile/link failure (with m_diagnostic set). Never
// touches m_id, so the caller (ctor or Reload) owns the adopt/rollback decision.
GLuint Shader::buildProgram(const char* vertexPath, const char* fragmentPath) {
    std::string vertexCode, fragmentCode;
    if (!LoadShaderSource(vertexPath, vertexCode, m_diagnostic) ||
        !LoadShaderSource(fragmentPath, fragmentCode, m_diagnostic)) {
        LUMINUMBRA_CORE_ERROR(
            "SHADER IO ERROR ({} / {}): {}", vertexPath, fragmentPath, m_diagnostic);
        return 0;
    }

    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

    // Compile Vertex Shader
    GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    const bool vertex_ok = checkCompileErrors(vertex, "VERTEX");

    // Compile Fragment Shader
    GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    const bool fragment_ok = checkCompileErrors(fragment, "FRAGMENT");

    if (!vertex_ok || !fragment_ok) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }

    // Link Program
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    const bool program_ok = checkCompileErrors(program, "PROGRAM");

    // Delete the shaders as they're linked into our program now and no longer necessary
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    if (!program_ok) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

Shader::Shader(const char* vertexPath, const char* fragmentPath) {
    m_debug_name = std::string(vertexPath) + " | " + fragmentPath;
    m_vertex_path = vertexPath;
    m_fragment_path = fragmentPath;

    m_id = buildProgram(vertexPath, fragmentPath);
    if (m_id == 0) {
        m_valid = false;
        return;
    }

    // introspect the resource layout once at link time.
    m_reflected = ReflectProgramLayout(m_id);
    m_valid = true;
}

Shader::~Shader() {
    if (m_id)
        glDeleteProgram(m_id);
}

void Shader::use() const {
    if (m_id)
        glUseProgram(m_id);
}

// validate the bindings a pass adopts against the reflected layout.
bool Shader::ValidateLayout(const ExpectedLayout& expected) {
    m_expected = expected;
    m_has_expected = true;
    const ValidationResult vr = ValidateReflectedLayout(m_reflected, expected);
    if (!vr.ok) {
        LUMINUMBRA_CORE_ERROR("[shader-reflect] {} -- {}", m_debug_name, vr.diagnostic);
    } else if (vr.had_warning) {
        LUMINUMBRA_CORE_WARN("[shader-reflect] {} -- {}", m_debug_name, vr.diagnostic);
    }
    return vr.ok;
}

// rollback-safe hot reload. Build a NEW program; only adopt it
// if it compiled, linked, and (when a pass expectation is registered) still
// matches that expectation. On any failure keep the previous good program.
bool Shader::Reload() {
    if (m_vertex_path.empty() || m_fragment_path.empty()) {
        LUMINUMBRA_CORE_WARN("[shader-reload] {} has no stored source paths; cannot reload.",
                             m_debug_name);
        return false;
    }

    const GLuint candidate = buildProgram(m_vertex_path.c_str(), m_fragment_path.c_str());
    if (candidate == 0) {
        LUMINUMBRA_CORE_ERROR(
            "[shader-reload] {} recompile FAILED; keeping previous program (id {}). {}",
            m_debug_name,
            m_id,
            m_diagnostic);
        return false; // rollback: previous m_id untouched
    }

    const ReflectedLayout candidate_layout = ReflectProgramLayout(candidate);
    if (m_has_expected) {
        const ValidationResult vr = ValidateReflectedLayout(candidate_layout, m_expected);
        if (!vr.ok) {
            LUMINUMBRA_CORE_ERROR("[shader-reload] {} reflected-layout MISMATCH after edit; "
                                  "ROLLING BACK to the previous good program. {}",
                                  m_debug_name,
                                  vr.diagnostic);
            glDeleteProgram(candidate);
            return false; // rollback: never adopt the broken layout
        }
    }

    // Adopt the new program: delete the old one, swap, refresh reflection, and
    // invalidate the uniform-location cache (locations change with relink).
    if (m_id)
        glDeleteProgram(m_id);
    m_id = candidate;
    m_reflected = candidate_layout;
    m_uniformLocationCache.clear();
    m_valid = true;
    LUMINUMBRA_CORE_INFO("[shader-reload] {} reloaded (id {}).", m_debug_name, m_id);
    return true;
}

// Helper function to get uniform location from cache or query it if not present
GLint Shader::getUniformLocation(const std::string& name) const {
    if (m_uniformLocationCache.find(name) != m_uniformLocationCache.end()) {
        return m_uniformLocationCache[name];
    }

    GLint location = glGetUniformLocation(m_id, name.c_str());
    m_uniformLocationCache[name] = location;
    return location;
}

// --- Uniform Setters ---
// All setters are now modified to use the caching helper function.

void Shader::setBool(const std::string& name, bool value) const {
    glUniform1i(getUniformLocation(name), (int)value);
}

void Shader::setInt(const std::string& name, int value) const {
    glUniform1i(getUniformLocation(name), value);
}

void Shader::setFloat(const std::string& name, float value) const {
    glUniform1f(getUniformLocation(name), value);
}

void Shader::setVec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec2(const std::string& name, float x, float y) const {
    glUniform2f(getUniformLocation(name), x, y);
}

void Shader::setVec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec3(const std::string& name, float x, float y, float z) const {
    glUniform3f(getUniformLocation(name), x, y, z);
}

void Shader::setVec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setVec4(const std::string& name, float x, float y, float z, float w) const {
    glUniform4f(getUniformLocation(name), x, y, z, w);
}

void Shader::setMat2(const std::string& name, const glm::mat2& mat) const {
    glUniformMatrix2fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat3(const std::string& name, const glm::mat3& mat) const {
    glUniformMatrix3fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const {
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

// checkCompileErrors implementation remains the same
bool Shader::checkCompileErrors(GLuint shader, const std::string& type) {
    GLint success;
    GLint log_length = 0;
    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
            std::vector<GLchar> info_log(static_cast<size_t>(std::max(log_length, 1)));
            glGetShaderInfoLog(
                shader, static_cast<GLsizei>(info_log.size()), nullptr, info_log.data());
            m_diagnostic = std::string(info_log.data());
            LUMINUMBRA_CORE_ERROR("SHADER_COMPILATION_ERROR of type: {0} ({1})\n{2}",
                                  type,
                                  m_debug_name,
                                  m_diagnostic);
            return false;
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramiv(shader, GL_INFO_LOG_LENGTH, &log_length);
            std::vector<GLchar> info_log(static_cast<size_t>(std::max(log_length, 1)));
            glGetProgramInfoLog(
                shader, static_cast<GLsizei>(info_log.size()), nullptr, info_log.data());
            m_diagnostic = std::string(info_log.data());
            LUMINUMBRA_CORE_ERROR(
                "PROGRAM_LINKING_ERROR of type: {0} ({1})\n{2}", type, m_debug_name, m_diagnostic);
            return false;
        }
    }
    return true;
}

} // namespace Luminumbra::Rendering
