#pragma once

#include "../../luminumbra_common/core/Log.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include <glad/glad.h>
#include <limits>
#include <stb_image.h>
#include <string>

class AssetManager {
public:
    static GLuint LoadTexture(const char* path) {
        GLuint textureID = 0;
        glGenTextures(1, &textureID);

        int width = 0, height = 0, nrComponents = 0;
        unsigned char* data = nullptr;
        if (Luminumbra::Filesystem::capture_content_paths().active()) {
            std::string bytes;
            if (Luminumbra::Filesystem::capture_content_paths().read_file(
                    path,
                    bytes,
                    static_cast<std::size_t>(std::numeric_limits<int>::max()),
                    Luminumbra::Filesystem::MissingContent::Optional))
                data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                             static_cast<int>(bytes.size()),
                                             &width,
                                             &height,
                                             &nrComponents,
                                             0);
        } else {
            data = stbi_load(path, &width, &height, &nrComponents, 0);
        }
        if (!data) {
            LUMINUMBRA_CORE_ERROR("Texture failed to load at path: {}", path);
            if (textureID)
                glDeleteTextures(1, &textureID);
            return 0;
        }

        GLenum srcFormat = GL_RGB;
        GLint internalFormat = GL_RGB8;
        if (nrComponents == 1) {
            srcFormat = GL_RED;
            internalFormat = GL_R8;
        }
        if (nrComponents == 3) {
            srcFormat = GL_RGB;
            internalFormat = GL_RGB8;
        }
        if (nrComponents == 4) {
            srcFormat = GL_RGBA;
            internalFormat = GL_RGBA8;
        }

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(
            GL_TEXTURE_2D, 0, internalFormat, width, height, 0, srcFormat, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
        return textureID;
    }
};
