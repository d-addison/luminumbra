#pragma once

// Only the platform-independent GLFW queries used by the production collector.
struct GLFWwindow;

inline constexpr int GLFW_FALSE = 0;
inline constexpr int GLFW_TRUE = 1;

extern "C" {
int glfwExtensionSupported(const char* extension);
void glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height);
}
