// include/luminumbra/rendering/Camera.h
#pragma once

#include <glm/glm.hpp>

namespace Luminumbra::Rendering {

class Camera {
public:
    Camera(float screenWidth, float screenHeight);

    void processMouseMovement(float xoffset, float yoffset);
    void processMouseScroll(float yoffset);
    
    const glm::mat4& getViewMatrix() const { return m_ViewMatrix; }
    const glm::mat4& getProjectionMatrix() const { return m_ProjectionMatrix; }
    const glm::vec3& getPosition() const { return m_Position; }
    const glm::vec3& getFront() const { return m_Front; }
    const glm::vec3& getRight() const { return m_Right; }
    const glm::vec3& getUp() const { return m_Up; }
    float getFov() const { return m_Fov; }

    void setPosition(const glm::vec3& position);
    void setFov(float fov);

    float getNearPlane() const { return m_NearPlane; }
    float getFarPlane() const { return m_FarPlane; }

private:
    void recalculateViewMatrix();
    void recalculateProjectionMatrix();
    void recalculateVectors();

    float m_ScreenWidth, m_ScreenHeight;
    glm::mat4 m_ProjectionMatrix;
    glm::mat4 m_ViewMatrix;
    
    glm::vec3 m_Position = glm::vec3(16.0f, 50.0f, 16.0f);
    glm::vec3 m_Front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 m_Up = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 m_Right;
    glm::vec3 m_WorldUp = m_Up;

    float m_Yaw = -90.0f;
    float m_Pitch = 0.0f;

    // Camera options
    float m_Fov = 45.0f;
    float m_NearPlane = 0.1f;
    float m_FarPlane = 1000.0f;
};

} // namespace Luminumbra::Rendering
