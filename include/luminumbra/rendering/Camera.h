// include/luminumbra/rendering/Camera.h
#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

class Camera {
public:
    Camera(float screenWidth, float screenHeight);

    void update(float mouseX, float mouseY);
    void processMouseMovement(float xoffset, float yoffset);
    void processMouseScroll(float yoffset);
    
    const glm::mat4& getViewMatrix() const { return m_ViewMatrix; }
    const glm::mat4& getProjectionMatrix() const { return m_ProjectionMatrix; }
    const glm::vec3& getPosition() const { return m_Position; }

private:
    void recalculateViewMatrix();

    glm::mat4 m_ProjectionMatrix;
    glm::mat4 m_ViewMatrix;
    
    glm::vec3 m_Position = glm::vec3(0.0f, 0.0f, 3.0f);
    glm::vec3 m_FocalPoint = glm::vec3(0.0f); // The point we are looking at
    glm::vec3 m_Up = glm::vec3(0.0f, 1.0f, 0.0f);

    float m_Yaw = -90.0f; // Horizontal angle
    float m_Pitch = 0.0f;  // Vertical angle
    float m_Distance = 3.0f;
};

} // namespace Luminumbra::Rendering