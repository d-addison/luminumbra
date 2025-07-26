// src/luminumbra/rendering/Camera.cpp
#include "luminumbra/rendering/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>

namespace Luminumbra::Rendering {

Camera::Camera(float screenWidth, float screenHeight) {
    // Set up the projection matrix
    // Field of View: 45 degrees
    // Aspect Ratio: screen width / screen height
    // Near clipping plane: 0.1 units
    // Far clipping plane: 100.0 units
    m_ProjectionMatrix = glm::perspective(glm::radians(45.0f), screenWidth / screenHeight, 0.1f, 100.0f);
    
    recalculateViewMatrix();
}

void Camera::update(float mouseX, float mouseY) {
    // A simple orbital camera implementation
    float sensitivity = 100.0f; // Adjust this value to your liking
    m_Yaw = mouseX * sensitivity;
    m_Pitch = -mouseY * sensitivity; // Invert Y-axis for standard controls

    // Clamp pitch to avoid flipping
    if (m_Pitch > 89.0f) m_Pitch = 89.0f;
    if (m_Pitch < -89.0f) m_Pitch = -89.0f;
    
    recalculateViewMatrix();
}

void Camera::recalculateViewMatrix() {
    // Calculate the camera's position on a sphere around the focal point
    m_Position.x = m_FocalPoint.x + m_Distance * cos(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
    m_Position.y = m_FocalPoint.y + m_Distance * sin(glm::radians(m_Pitch));
    m_Position.z = m_FocalPoint.z + m_Distance * sin(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));

    // The view matrix positions and orients the entire world relative to the camera
    m_ViewMatrix = glm::lookAt(m_Position, m_FocalPoint, m_Up);
}

} // namespace Luminumbra::Rendering