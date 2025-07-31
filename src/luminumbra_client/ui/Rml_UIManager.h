#pragma once
#include "Rml_Interfaces.h"
#include <RmlUi/Core.h>
#include <string>

class IAudioManager;
struct GLFWwindow;

namespace Luminumbra::Client {

class Rml_UIManager {
public:
    Rml_UIManager();
    ~Rml_UIManager();

    void Init(GLFWwindow* window, IAudioManager* audioManager);
    void Shutdown();

    void Update();
    void Render();

    Rml::Context* GetContext();
    void LoadDocument(const std::string& rml_path);
    
    // Static input handlers to be called by GLFW
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

private:
    RmlSystem m_systemInterface;
    RmlRenderer m_renderInterface;
    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;

    static Rml::Context* s_activeContext;
};

} // namespace Luminumbra::Client