#pragma once

#include <string>
#include <iostream>
#include <glad/gl.h>

namespace Luminumbra::Core {

namespace Detail {
    // Move the functions into a Detail namespace
    inline void GLClearError() {
        while (glGetError() != GL_NO_ERROR);
    }

    inline bool GLCheckError(const std::string& function, const std::string& file, int line) {
        bool hasError = false;
        while (GLenum error = glGetError()) {
            std::string errorString;
            switch (error) {
                case GL_INVALID_ENUM: errorString = "GL_INVALID_ENUM"; break;
                case GL_INVALID_VALUE: errorString = "GL_INVALID_VALUE"; break;
                case GL_INVALID_OPERATION: errorString = "GL_INVALID_OPERATION"; break;
                case GL_OUT_OF_MEMORY: errorString = "GL_OUT_OF_MEMORY"; break;
                case GL_INVALID_FRAMEBUFFER_OPERATION: errorString = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
                default: errorString = "Unknown error"; break;
            }
            
            std::cerr << "[OpenGL Error] (" << error << " - " << errorString << "): " 
                      << function << " " << file << ":" << line << std::endl;
            hasError = true;
        }
        return hasError;
    }
}

// Define the macro to use the Detail namespace
#define GLCall(x) do { \
    Luminumbra::Core::Detail::GLClearError();\
    x;\
    Luminumbra::Core::Detail::GLCheckError(#x, __FILE__, __LINE__);\
} while(0)

} // namespace Luminumbra::Core