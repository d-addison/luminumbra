#ifndef DEBUG_H
#define DEBUG_H

#include <iostream>
#define LOG(message) std::cout << message << std::endl;

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "luminumbra/rendering/Shader.h"

namespace Luminumbra::Debug {

class Debug {
public:
    Debug();
    ~Debug();

    void drawAxes(const glm::mat4& projection, const glm::mat4& view);

private:
    Luminumbra::Rendering::Shader shader;
    GLuint VAO, VBO;

    void setupAxes();
};

} // namespace Luminumbra::Debug

#endif // DEBUG_H
