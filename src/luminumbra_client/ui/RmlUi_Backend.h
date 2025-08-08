// src/luminumbra_client/ui/RmlUi_Backend.h
#pragma once

struct GLFWwindow;

namespace Backend {

// Initializes the backend, setting up the render interface and system interface.
void Initialize(GLFWwindow* window);

// Shuts down the backend and releases all resources.
void Shutdown();

// Must be called before Rml::Context::Update and Rml::Context::Render.
void BeginFrame();

// Must be called after Rml::Context::Update and Rml::Context::Render.
void EndFrame();

} // namespace Backend
