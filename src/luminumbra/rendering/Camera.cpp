// src/luminumbra/rendering/Camera.cpp
#include "luminumbra/rendering/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

Camera::Camera(float screenWidth, float screenHeight) {
    m_ProjectionMatrix = glm::perspective(glm::radians(45.0f), screenWidth / screenHeight, 0.1f, 1000.0f);
    recalculateVectors();
    recalculateViewMatrix();
}

void Camera::processMouseMovement(float xoffset, float yoffset) {
    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    m_Yaw += xoffset;
    m_Pitch += yoffset;

    if (m_Pitch > 89.0f)
        m_Pitch = 89.0f;
    if (m_Pitch < -89.0f)
        m_Pitch = -89.0f;

    recalculateVectors();
    recalculateViewMatrix();
}

void Camera::setPosition(const glm::vec3& position) {
    m_Position = position;
    recalculateViewMatrix();
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

void Camera::recalculateViewMatrix() {
    m_ViewMatrix = glm::lookAt(m_Position, m_Position + m_Front, m_Up);
}

} // namespace Luminumbra::Rendering
