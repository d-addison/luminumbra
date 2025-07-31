#include "ui/Rml_UIManager.h"
#include "audio/IAudioManager.h" 
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Debugger.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>

#define SOIL_IMPLEMENTATION
#include <SOIL2/SOIL2.h> 

#include <iostream>

namespace Luminumbra::Client {

// --- RmlRenderer Implementation ---

RmlRenderer::RmlRenderer() {
    // Minimal shaders for UI rendering
    const char* vs = R"GLSL(
        #version 330 core
        in vec2 aPos;
        in vec2 aTexCoord;
        in vec4 aColor;
        out vec2 TexCoord;
        out vec4 FragColor;
        uniform mat4 uProjection;
        uniform vec2 uTranslation;
        void main() {
            FragColor = aColor;
            TexCoord = aTexCoord;
            gl_Position = uProjection * vec4(aPos + uTranslation, 0.0, 1.0);
        }
    )GLSL";
    const char* fs = R"GLSL(
        #version 330 core
        in vec2 TexCoord;
        in vec4 FragColor;
        out vec4 OutColor;
        uniform sampler2D uTexture;
        void main() {
            vec4 texColor = texture(uTexture, TexCoord);
            OutColor = FragColor * texColor;
        }
    )GLSL";

    // Compile and link shaders
    GLuint vertShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertShader, 1, &vs, NULL);
    glCompileShader(vertShader);

    GLuint fragShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragShader, 1, &fs, NULL);
    glCompileShader(fragShader);

    m_program = glCreateProgram();
    glAttachShader(m_program, vertShader);
    glAttachShader(m_program, fragShader);
    glLinkProgram(m_program);

    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    m_translation_loc = glGetUniformLocation(m_program, "uTranslation");
    m_projection_loc = glGetUniformLocation(m_program, "uProjection");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
}

void RmlRenderer::SetViewport(int width, int height) {
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}

void RmlRenderer::RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture, const Rml::Vector2f& translation) {
    glUseProgram(m_program);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Rml::Vertex) * num_vertices, vertices, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0); // aPos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, position));
    glEnableVertexAttribArray(1); // aTexCoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, tex_coord));
    glEnableVertexAttribArray(2); // aColor
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Rml::Vertex), (void*)offsetof(Rml::Vertex, colour));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(int) * num_indices, indices, GL_DYNAMIC_DRAW);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glm::mat4 projection = glm::ortho(0.0f, (float)m_width, (float)m_height, 0.0f, -1.0f, 1.0f);
    glUniformMatrix4fv(m_projection_loc, 1, GL_FALSE, &projection[0][0]);
    glUniform2fv(m_translation_loc, 1, &translation.x);
    
    if (texture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glDrawElements(GL_TRIANGLES, num_indices, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void RmlRenderer::EnableScissorRegion(bool enable) {
    if (enable)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
}

void RmlRenderer::SetScissorRegion(int x, int y, int width, int height) {
    glScissor(x, m_height - (y + height), width, height);
}

bool RmlRenderer::LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) {
    // Our TDD specifies data comes from the `data/` dir, not `assets/`
    Rml::String full_path = "data/" + source;
    
    int width, height, channels;
    unsigned char* image_data = SOIL_load_image(full_path.c_str(), &width, &height, &channels, SOIL_LOAD_RGBA);
    if (!image_data) {
        std::cerr << "RmlUi ERROR: Could not load texture: " << full_path << std::endl;
        return false;
    }

    texture_dimensions = { width, height };
    return GenerateTexture(texture_handle, image_data, texture_dimensions);
}

bool RmlRenderer::GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) {
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0) return false;

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, source_dimensions.x, source_dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    texture_handle = (Rml::TextureHandle)texture_id;
    return true;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture_handle) {
    glDeleteTextures(1, (GLuint*)&texture_handle);
}

// --- Rml_UIManager Implementation ---

Rml::Context* Rml_UIManager::s_activeContext = nullptr;

Rml_UIManager::Rml_UIManager() {}
Rml_UIManager::~Rml_UIManager() {}

void Rml_UIManager::Init(GLFWwindow* window, IAudioManager* audioManager) {
    m_window = window;
    m_audioManager = audioManager;
    std::cout << "UI Manager Initializing..." << std::endl;

    Rml::SetSystemInterface(&m_systemInterface);
    Rml::SetRenderInterface(&m_renderInterface);

    Rml::Initialise();

    // Load fonts - This makes the entire "Lora" family available to CSS
    bool fonts_loaded = true;
    fonts_loaded &= Rml::LoadFontFace("assets/fonts/Lora/static/Lora-Regular.ttf",      false);
    fonts_loaded &= Rml::LoadFontFace("assets/fonts/Lora/static/Lora-Italic.ttf",        true);
    fonts_loaded &= Rml::LoadFontFace("assets/fonts/Lora/static/Lora-Bold.ttf",          false);
    fonts_loaded &= Rml::LoadFontFace("assets/fonts/Lora/static/Lora-BoldItalic.ttf",    true);

    if (!fonts_loaded) {
        std::cerr << "UI WARNING: Failed to load one or more Lora font faces. Check paths." << std::endl;
    }

    int width, height;
    glfwGetWindowSize(m_window, &width, &height);

    // Set Lora as the default font family
    // The second parameter 'false' means it's a fallback font.
    Rml::SetFontFamily("Lora", "Lora"); 

    m_context = Rml::CreateContext("main", Rml::Vector2i(width, height));
    s_activeContext = m_context; // For static callbacks
    Rml::Debugger::Initialise(m_context);

    // Set up GLFW callbacks for RmlUi
    glfwSetKeyCallback(m_window, Rml_UIManager::KeyCallback);
    glfwSetCharCallback(m_window, Rml_UIManager::CharCallback);
    glfwSetMouseButtonCallback(m_window, Rml_UIManager::MouseButtonCallback);
    glfwSetCursorPosCallback(m_window, Rml_UIManager::CursorPosCallback);
    glfwSetScrollCallback(m_window, Rml_UIManager::ScrollCallback);

    std::cout << "UI Manager Initialized." << std::endl;
}

void Rml_UIManager::Shutdown() {
    Rml::Shutdown();
    s_activeContext = nullptr;
    std::cout << "UI Manager Shutdown." << std::endl;
}

void Rml_UIManager::Update() {
    if (m_context) {
        int width, height;
        glfwGetWindowSize(m_window, &width, &height);
        m_context->SetDimensions(Rml::Vector2i(width, height));
        m_renderInterface.SetViewport(width, height);
        m_context->Update();
    }
}

void Rml_UIManager::Render() {
    if (m_context) {
        m_context->Render();
    }
}

Rml::Context* Rml_UIManager::GetContext() {
    return m_context;
}

class QuitButtonListener : public Rml::EventListener {
public:
    QuitButtonListener(GLFWwindow* window) : m_window(window) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "click") {
            glfwSetWindowShouldClose(m_window, true);
        }
    }
private:
    GLFWwindow* m_window;
};

class ButtonSoundListener : public Rml::EventListener {
public:
    ButtonSoundListener(IAudioManager* audio) : m_audio(audio) {}
    void ProcessEvent(Rml::Event& event) override {
        if (event.GetType() == "mouseover" && m_audio) {
            m_audio->PlayOneShot2D("ui_button_click");
        }
    }
private:
    IAudioManager* m_audio;
};

void Rml_UIManager::LoadDocument(const std::string& rml_path) {
    if (!m_context) return;
    
    // Unload previous documents if any
    for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
        m_context->GetDocument(i)->Close();
    }
    
    Rml::ElementDocument* document = m_context->LoadDocument("data/ui/" + rml_path);
    if (document) {
        document->Show();

        // Add event listeners
        Rml::Element* quit_button = document->GetElementById("quit_button");
        if (quit_button) {
            quit_button->AddEventListener("click", new QuitButtonListener(m_window));
        }
        
        // Add sound to all buttons
        Rml::ElementList buttons;
        document->GetElementsByTagName(buttons, "button");
        for(Rml::Element* button : buttons) {
            // FIX: Pass the audio manager to the listener's constructor
            button->AddEventListener("mouseover", new ButtonSoundListener(m_audioManager));
        }

    } else {
        std::cerr << "UI ERROR: Could not load document: " << rml_path << std::endl;
    }
}

// GLFW Input Callbacks Implementation
Rml::Input::KeyIdentifier GlfwToRmlKey(int key) { /* Maps GLFW keys to RmlUi keys, implementation omitted for brevity but is required */ return Rml::Input::KI_UNKNOWN; }
int GlfwToRmlMods(int mods) { /* Maps GLFW mods, implementation omitted for brevity */ return 0; }

void Rml_UIManager::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if(!s_activeContext) return;
    // ... (Full implementation would map GLFW keys to RmlUi keys)
    if (key == GLFW_KEY_F8 && action == GLFW_PRESS) {
         Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible());
    }
}
void Rml_UIManager::CharCallback(GLFWwindow* window, unsigned int codepoint) {
    if (s_activeContext) s_activeContext->ProcessTextInput(codepoint);
}
void Rml_UIManager::MouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    if (s_activeContext) {
        if (action == GLFW_PRESS) s_activeContext->ProcessMouseButtonDown(button, GlfwToRmlMods(mods));
        else s_activeContext->ProcessMouseButtonUp(button, GlfwToRmlMods(mods));
    }
}
void Rml_UIManager::CursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    if (s_activeContext) s_activeContext->ProcessMouseMove( (int)xpos, (int)ypos, 0);
}
void Rml_UIManager::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    if (s_activeContext) s_activeContext->ProcessMouseWheel( (float)-yoffset, 0);
}


} // namespace Luminumbra::Client