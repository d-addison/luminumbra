// Shared GL compute harness for the  GPU benches ( profile +
//  parity). Hidden GL 4.5 core context, compute-program compile, SSBO
// helpers, renderer-string + software-renderer detection. Header-only; depends
// on glad + glfw. Kept separate from shieldrt_far_field.h (which is the terrain
// data/builders) so a bench pulls exactly the harness it needs.

#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luminumbra_shieldrt {

// Hidden GL 4.5 core context (matches render_smoke_test's harness). ready is
// false on a headless machine (the bench then GTEST_SKIPs).
class HiddenGlContext {
public:
    explicit HiddenGlContext(const char* title) {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(64, 64, title, nullptr, nullptr);
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

inline std::string GlString(GLenum name) {
    const GLubyte* s = glGetString(name);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
}

inline bool IsSoftwareRenderer(const std::string& renderer) {
    std::string r = renderer;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ||
           r.find("software") != std::string::npos || r.find("swiftshader") != std::string::npos ||
           (r.find("microsoft") != std::string::npos && r.find("warp") != std::string::npos);
}

// Compile + link a compute program. Returns 0 and fills `error` on failure.
inline GLuint CompileComputeProgram(const char* src, std::string& error) {
    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        error = "compute compile failed: " + log;
        glDeleteShader(shader);
        return 0;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    glDeleteShader(shader);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        error = "compute link failed: " + log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

inline GLuint MakeStorageBuffer(GLsizeiptr bytes, const void* data) {
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, data, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return buf;
}

inline void SetIntArray(GLuint prog, const char* name, const std::int32_t* data, int count) {
    const GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0)
        glUniform1iv(loc, count, data);
}

inline double MedianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace luminumbra_shieldrt
