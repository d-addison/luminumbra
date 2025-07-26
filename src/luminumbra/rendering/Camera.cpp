// src/luminumbra/rendering/Camera.cpp
#include "luminumbra/rendering/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>

namespace Luminumbra::Rendering {

Camera::Camera(float screenWidth, float screenHeight) {
    m_ProjectionMatrix = glm::perspective(glm::radians(45.0f), screenWidth / screenHeight, 0.1f, 1000.0f); // Increase far plane

    // --- NEW STARTING VALUES ---
    m_FocalPoint = glm::vec3(16.0f, 0.0f, 16.0f); // Look at the center of a chunk
    m_Distance = 100.0f; // Start further away
    m_Pitch = 30.0f; // Start with a 30-degree downward angle
    m_Yaw = 45.0f;   // Start at a 45-degree side angle
    
    recalculateViewMatrix();
}

// NEW MOUSE HANDLING LOGIC
void Camera::processMouseMovement(float xoffset, float yoffset) {
    float sensitivity = 0.2f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    m_Yaw += xoffset;
    m_Pitch += yoffset;

    // Clamp pitch
    if (m_Pitch > 89.0f)
        m_Pitch = 89.0f;
    if (m_Pitch < -89.0f)
        m_Pitch = -89.0f;
    
    recalculateViewMatrix();
}

void Camera::processMouseScroll(float yoffset) {
    m_Distance -= yoffset * 5.0f; // Zoom in/out
    if (m_Distance < 1.0f)
        m_Distance = 1.0f;
    if (m_Distance > 1000.0f)
        m_Distance = 1000.0f;
    
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
