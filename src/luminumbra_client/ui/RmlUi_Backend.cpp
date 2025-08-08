// src/luminumbra_client/ui/RmlUi_Backend.cpp
#include "RmlUi_Backend.h"
#include <RmlUi/Core.h>
#include "Rml_Interfaces.h"
#include <GLFW/glfw3.h>

// --- Backend Data ---
static Luminumbra::Client::RmlSystem g_system_interface;
static Luminumbra::Client::RmlRenderer g_render_interface;

// --- Backend Implementation ---
void Backend::Initialize(GLFWwindow* window)
{
    // Set the interfaces
    Rml::SetSystemInterface(&g_system_interface);
    Rml::SetRenderInterface(&g_render_interface);
}

void Backend::Shutdown()
{
    // The implementation objects are static, so they will be destroyed automatically.
}

void Backend::BeginFrame()
{
    int width, height;
    glfwGetFramebufferSize(glfwGetCurrentContext(), &width, &height);
    
    g_render_interface.SetViewport(width, height);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void Backend::EndFrame()
{
    glDisable(GL_SCISSOR_TEST);
}
