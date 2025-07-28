#include "luminumbra/rendering/TextureLoader.h"
#include "luminumbra/core/GLError.h"
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace Luminumbra::Rendering {

GLuint loadTexture(const std::string& path) {
    stbi_set_flip_vertically_on_load(true);
    GLuint textureID;
    GLCall(glGenTextures(1, &textureID));

    int width, height, nrComponents;
    unsigned char *data = stbi_load(path.c_str(), &width, &height, &nrComponents, 0);
    if (data) {
        GLenum format;
        if (nrComponents == 1)
            format = GL_RED;
        else if (nrComponents == 3)
            format = GL_RGB;
        else if (nrComponents == 4)
            format = GL_RGBA;
        else {
            std::cerr << "TextureLoader Error: Unsupported number of components (" << nrComponents << ") in " << path << std::endl;
            stbi_image_free(data);
            return 0;
        }

        GLCall(glBindTexture(GL_TEXTURE_2D, textureID));
        GLCall(glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data));
        GLCall(glGenerateMipmap(GL_TEXTURE_2D));

        GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
        GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
        GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
        GLCall(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

        stbi_image_free(data);
    } else {
        std::cerr << "TextureLoader Error: Failed to load texture at path: " << path << std::endl;
        std::cerr << "STB Failure Reason: " << stbi_failure_reason() << std::endl;
        return 0;
    }

    return textureID;
}

} // namespace Luminumbra::Rendering