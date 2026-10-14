#pragma once

#include "../FarWorldSelection.h"

#include <glad/glad.h>

namespace Luminumbra::Rendering {

class Shader;

// Fixed location storage owned by GBufferPass. Recache only on a changed Shader
// generation; ordinary frame/tile uploads do not look up locations or allocate.
// The Shader owner must outlive this observer (or detach it with Cache(0)).
class FarWorldPassUniforms {
public:
    void Cache(GLuint program) noexcept;
    void Cache(const Shader& shader) noexcept;
    void Reset() noexcept;
    void Begin() noexcept;
    void UseInstanceOrigin(bool enabled) noexcept;
    bool Prepare(const FarWorldDrawCommand& command) noexcept;

private:
    void Refresh() noexcept;
    void CacheLocations(GLuint program) noexcept;
    const Shader* shader_ = nullptr;
    std::uint64_t generation_ = 0;
    GLuint program_ = 0;
    GLint ladder_ = -1, bias_ = -1, near_ = -1, far_ = -1, instance_ = -1;
    GLint outer_ = -1, edge_ = -1, arrival_ = -1, finer_outer_ = -1, children_ = -1;
    GLint chunk_ = -1, remainder_ = -1, count_ = -1, relative_ = -1, world_ = -1;
    bool first_ = true;
    float tier_edge_ = 0;
};

class FarWorldPassUniformScope {
public:
    explicit FarWorldPassUniformScope(FarWorldPassUniforms& uniforms) noexcept
        : uniforms_(uniforms) {
        uniforms_.Begin();
    }
    ~FarWorldPassUniformScope() {
        uniforms_.Reset();
    }
    FarWorldPassUniformScope(const FarWorldPassUniformScope&) = delete;
    FarWorldPassUniformScope& operator=(const FarWorldPassUniformScope&) = delete;

private:
    FarWorldPassUniforms& uniforms_;
};
} // namespace Luminumbra::Rendering
