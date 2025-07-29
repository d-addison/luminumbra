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
    float getPitch() const { return m_Pitch; }
    void setPitch(float pitch);

    void updateFrustum() {
        glm::mat4 vp = m_ProjectionMatrix * m_ViewMatrix;
        // Left plane
        m_FrustumPlanes[0] = glm::vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
        // Right plane
        m_FrustumPlanes[1] = glm::vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
        // Bottom plane
        m_FrustumPlanes[2] = glm::vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
        // Top plane
        m_FrustumPlanes[3] = glm::vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
        // Near plane
        m_FrustumPlanes[4] = glm::vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]);
        // Far plane
        m_FrustumPlanes[5] = glm::vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);

        // Normalize the planes
        for (int i = 0; i < 6; ++i) {
            m_FrustumPlanes[i] = glm::normalize(m_FrustumPlanes[i]);
        }
    }

    // ADD THIS
    bool isBoxInFrustum(const glm::vec3& min, const glm::vec3& max) const {
        for (int i = 0; i < 6; ++i) {
            glm::vec3 p = min;
            if (m_FrustumPlanes[i].x >= 0) p.x = max.x;
            if (m_FrustumPlanes[i].y >= 0) p.y = max.y;
            if (m_FrustumPlanes[i].z >= 0) p.z = max.z;

            if (glm::dot(glm::vec3(m_FrustumPlanes[i]), p) + m_FrustumPlanes[i].w < 0) {
                return false;
            }
        }
        return true;
    }

private:
    void recalculateViewMatrix();
    void recalculateProjectionMatrix();
    void recalculateVectors();

    glm::vec4 m_FrustumPlanes[6];

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
