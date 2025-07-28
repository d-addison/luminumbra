#include "luminumbra/rendering/Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace Luminumbra::Rendering {

Camera::Camera(float screenWidth, float screenHeight)
    : m_ScreenWidth(screenWidth), m_ScreenHeight(screenHeight) {
    recalculateProjectionMatrix();
    recalculateVectors();
    recalculateViewMatrix();
}

void Camera::processMouseMovement(float xoffset, float yoffset) {
    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    m_Yaw += xoffset;
    m_Pitch += yoffset;

    // Clamp pitch
    if (m_Pitch > 89.0f)
        m_Pitch = 89.0f;
    if (m_Pitch < -89.0f)
        m_Pitch = -89.0f;

    recalculateVectors();
    recalculateViewMatrix();
}

void Camera::processMouseScroll(float yoffset) {
    m_Fov -= (float)yoffset;
    if (m_Fov < 1.0f)
        m_Fov = 1.0f;
    if (m_Fov > 90.0f)
        m_Fov = 90.0f;
    
    recalculateProjectionMatrix();
}

void Camera::setPosition(const glm::vec3& position) {
    m_Position = position;
    recalculateViewMatrix();
}

void Camera::setPitch(float pitch) {
    m_Pitch = glm::clamp(pitch, -89.0f, 89.0f);
    recalculateVectors();
    recalculateViewMatrix();
}

void Camera::setFov(float fov) {
    m_Fov = fov;
    recalculateProjectionMatrix();
}

void Camera::recalculateViewMatrix() {
    m_ViewMatrix = glm::lookAt(m_Position, m_Position + m_Front, m_Up);
}

void Camera::recalculateProjectionMatrix() {
    m_ProjectionMatrix = glm::perspective(glm::radians(m_Fov), m_ScreenWidth / m_ScreenHeight, m_NearPlane, m_FarPlane);
}

void Camera::recalculateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
    front.y = sin(glm::radians(m_Pitch));
    front.z = sin(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
    m_Front = glm::normalize(front);
    m_Right = glm::normalize(glm::cross(m_Front, m_WorldUp));
    m_Up = glm::normalize(glm::cross(m_Right, m_Front));
}

} // namespace Luminumbra::Rendering
