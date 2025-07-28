#pragma once
#include <string>
#include <glad/gl.h>

namespace Luminumbra::Rendering {

// Loads a 2D texture from a file.
// Returns the OpenGL texture ID.
GLuint loadTexture(const std::string& path);

} // namespace Luminumbra::Rendering