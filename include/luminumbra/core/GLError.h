// include/luminumbra/core/GLError.h
#pragma once

#include <string>
#include <iostream>
#include <glad/gl.h>

namespace Luminumbra::Core {

// Clear all OpenGL errors
inline void GLClearError() {
    while (glGetError() != GL_NO_ERROR);
}

// Check for OpenGL errors and log them
inline bool GLCheckError(const std::string& function, const std::string& file, int line) {
    bool hasError = false;
    while (GLenum error = glGetError()) {
        std::string errorString;
        switch (error) {
            case GL_INVALID_ENUM:
                errorString = "GL_INVALID_ENUM";
                break;
            case GL_INVALID_VALUE:
                errorString = "GL_INVALID_VALUE";
                break;
            case GL_INVALID_OPERATION:
                errorString = "GL_INVALID_OPERATION";
                break;
            case GL_OUT_OF_MEMORY:
                errorString = "GL_OUT_OF_MEMORY";
                break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                errorString = "GL_INVALID_FRAMEBUFFER_OPERATION";
                break;
            default:
                errorString = "Unknown error";
                break;
        }
        
        std::cerr << "[OpenGL Error] (" << error << " - " << errorString << "): " 
                  << function << " " << file << ":" << line << std::endl;
        hasError = true;
    }
    return hasError;
}

// Macro for easy error checking
#define GLCall(x) GLClearError();\
    x;\
    GLCheckError(#x, __FILE__, __LINE__)

} // namespace Luminumbra::Core
