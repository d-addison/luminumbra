#pragma once

#include <glad/glad.h>

namespace Luminumbra::Rendering {

// Value-only caller contract; never serialized or used for simulation identity.
struct PassDepthState {
    GLenum function = GL_LESS;
    GLboolean write_mask = GL_TRUE;
    GLboolean test_enabled = GL_FALSE;

    // Only external callers with unknown GL state need driver queries.
    static PassDepthState QueryExternal() {
        GLint previous_function = 0;
        GLboolean previous_mask = GL_FALSE;
        glGetIntegerv(GL_DEPTH_FUNC, &previous_function);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &previous_mask);
        return {.function = static_cast<GLenum>(previous_function),
                .write_mask = previous_mask,
                .test_enabled = glIsEnabled(GL_DEPTH_TEST)};
    }

    void Restore() const {
        glDepthFunc(function);
        glDepthMask(write_mask);
        if (test_enabled)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
    }
};

} // namespace Luminumbra::Rendering
