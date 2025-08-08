#pragma once
#include "Rml_Interfaces.h"
#include <RmlUi/Core.h>
#include <string>
#include <functional>
#include <memory>
#include <vector>

struct GLFWwindow;

namespace Luminumbra::Client {

class IAudioManager;
class CreateWorldButtonListener;

using WorldCreationCallback = std::function<void(const std::string&, const std::string&, const std::string&)>;

class Rml_UIManager {
    friend class CreateWorldButtonListener;
public:
    Rml_UIManager(const std::string& root_path);
    ~Rml_UIManager();

    void Init(GLFWwindow* window, IAudioManager* audioManager);
    void Shutdown();

    void Update();
    void Render();

    Rml::Context* GetContext();
    void LoadDocument(const std::string& rml_path);

    void SetWorldCreationCallback(WorldCreationCallback callback) { m_worldCreationCallback = std::move(callback); }

    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void CharCallback(GLFWwindow* window, unsigned int codepoint);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

private:
    template <typename T, typename... Args>
    T* MakeListener(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = ptr.get();
        m_activeListeners.emplace_back(std::move(ptr));
        return raw;
    }

    RmlSystem m_systemInterface;
    RmlFileInterface m_fileInterface;
    RmlRenderer m_renderInterface;
    Rml::Context* m_context = nullptr;
    GLFWwindow* m_window = nullptr;
    IAudioManager* m_audioManager = nullptr;
    WorldCreationCallback m_worldCreationCallback;

    static Rml::Context* s_activeContext;

    // NEW: own all listeners attached to current document(s)
    std::vector<std::unique_ptr<Rml::EventListener>> m_activeListeners;
};

} // namespace Luminumbra::Client