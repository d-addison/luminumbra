#pragma once

#include "luminumbra_client/app/RenderMeasurement.h"
#include <glad/glad.h>

namespace Luminumbra::Rendering {
struct GlBenchmarkTimestamps {
    static bool supported() {
        GLint bits = 0;
        if (glGetQueryiv && glQueryCounter && glGetQueryObjectui64v)
            glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
        return bits > 0 && glGenQueries && glDeleteQueries && glGetQueryObjectiv;
    }
    static void create(std::uint32_t* ids, std::size_t count) {
        glGenQueries(static_cast<GLsizei>(count), ids);
    }
    static void destroy(const std::uint32_t* ids, std::size_t count) {
        glDeleteQueries(static_cast<GLsizei>(count), ids);
    }
    static void stamp(std::uint32_t id) {
        glQueryCounter(id, GL_TIMESTAMP);
    }
    static bool available(std::uint32_t id) {
        GLint ready = GL_FALSE;
        glGetQueryObjectiv(id, GL_QUERY_RESULT_AVAILABLE, &ready);
        return ready == GL_TRUE;
    }
    static std::uint64_t result(std::uint32_t id) {
        GLuint64 value = 0;
        glGetQueryObjectui64v(id, GL_QUERY_RESULT, &value);
        return value;
    }
};
using BenchmarkGpuQueries = Client::Measurement::GpuQueryRing<GlBenchmarkTimestamps>;
} // namespace Luminumbra::Rendering
