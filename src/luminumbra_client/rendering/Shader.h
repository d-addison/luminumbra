#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map> // Added for the cache

#include "ShaderReflection.h" // reflect resource layout at load time

namespace Luminumbra::Rendering {

class Shader {
public:
    Shader(const char* vertexPath, const char* fragmentPath);
    ~Shader();

    void use() const;
    bool IsValid() const {
        return m_valid && m_id != 0;
    }
    GLuint Id() const {
        return m_id;
    }
    const std::string& DebugName() const {
        return m_debug_name;
    }
    const std::string& Diagnostic() const {
        return m_diagnostic;
    }
    //  (live shader authoring): the stored source paths, exposed so the
    // auto-reload watcher can mtime-poll them. Empty for non-file-backed programs.
    const std::string& VertexPath() const {
        return m_vertex_path;
    }
    const std::string& FragmentPath() const {
        return m_fragment_path;
    }

    // ---: shader-resource reflection + layout validation --------
    // The resource layout introspected from the linked program (samplers / UBO /
    // SSBO / outputs). Empty until a successful link.
    const ReflectedLayout& Reflected() const {
        return m_reflected;
    }

    // Validate the bindings a PASS adopts (declared as an ExpectedLayout) against
    // the reflected layout. A hard mismatch (wrong sampler type, wrong binding
    // point) is logged at ERROR and returns false -- a load-time tripwire for the
    // "renders garbage" class. Absence-only (the linker stripped an unused
    // sampler) is logged at WARN and returns true. The expected layout is
    // remembered so a later hot-reload re-validates against it and
    // rolls back on mismatch.
    bool ValidateLayout(const ExpectedLayout& expected);

    //  hot-reload safety: recompile+relink from the stored source paths
    // into a SEPARATE program, reflect it, and (if an expected layout was
    // registered) re-validate. Only on full success is the new program swapped in
    // and the old one deleted; on ANY failure (IO / compile / link / reflected-
    // layout mismatch) the new program is discarded and the previous good program
    // is KEPT -- never adopt a broken program. Returns true iff a swap happened.
    bool Reload();

    void setBool(const std::string& name, bool value) const;
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec2(const std::string& name, const glm::vec2& value) const;
    void setVec2(const std::string& name, float x, float y) const;
    void setVec3(const std::string& name, const glm::vec3& value) const;
    void setVec3(const std::string& name, float x, float y, float z) const;
    void setVec4(const std::string& name, const glm::vec4& value) const;
    void setVec4(const std::string& name, float x, float y, float z, float w) const;
    void setMat2(const std::string& name, const glm::mat2& mat) const;
    void setMat3(const std::string& name, const glm::mat3& mat) const;
    void setMat4(const std::string& name, const glm::mat4& mat) const;

private:
    bool checkCompileErrors(GLuint shader, const std::string& type);
    GLint getUniformLocation(const std::string& name) const; // Helper to use the cache

    // Single compile+link path shared by the ctor and Reload. Reads the two
    // source files, compiles+links, and returns a NEW program name (0 on any
    // failure; m_diagnostic holds the reason). Does NOT touch m_id -- the caller
    // decides whether to adopt it, which is what makes rollback safe.
    GLuint buildProgram(const char* vertexPath, const char* fragmentPath);

    GLuint m_id = 0;
    bool m_valid = false;
    std::string m_debug_name;
    std::string m_diagnostic;
    // The cache for uniform locations. mutable allows it to be modified in const functions.
    mutable std::unordered_map<std::string, GLint> m_uniformLocationCache;

    // stored source paths (for Reload), the reflected resource
    // layout, and the optional pass-declared expectation re-checked on reload.
    std::string m_vertex_path;
    std::string m_fragment_path;
    ReflectedLayout m_reflected;
    ExpectedLayout m_expected;
    bool m_has_expected = false;
};

} // namespace Luminumbra::Rendering
