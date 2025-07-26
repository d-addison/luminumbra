#include "luminumbra/ui/UIManager.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

namespace Luminumbra {
    namespace UI {

        UIManager::UIManager(GLFWwindow* window) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO(); (void)io;
            ImGui::StyleColorsDark();
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 130");
        }

        UIManager::~UIManager() {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }

        void UIManager::NewFrame() {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }

        void UIManager::Render() {
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        void UIManager::ShowMainMenu() {
            if (!m_ShowMainMenu) return;

            ImGui::Begin("Main Menu", &m_ShowMainMenu);
            if (ImGui::Button("Start Game")) {
                m_ShowMainMenu = false;
            }
            if (ImGui::Button("Settings")) {
                m_ShowSettingsWindow = true;
            }
            if (ImGui::Button("Quit")) {
                // This is tricky. We need a way to signal the engine to close.
                // For now, we'll just hide the menu.
                m_ShowMainMenu = false;
            }
            ImGui::End();
        }

        void UIManager::ShowSettingsWindow() {
            if (!m_ShowSettingsWindow) return;

            ImGui::Begin("Settings", &m_ShowSettingsWindow);
            ImGui::Text("Settings go here.");
            if (ImGui::Button("Close")) {
                m_ShowSettingsWindow = false;
            }
            ImGui::End();
        }

        void UIManager::ShowDebugOverlay(double x, double y, double z, double acceleration, double gravity, const char* controllerMode) {
            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            if (ImGui::Begin("Debug Info", nullptr, window_flags)) {
                ImGui::Text("Debug Information");
                ImGui::Separator();
                ImGui::Text("Position (XYZ): %.2f, %.2f, %.2f", x, y, z);
                ImGui::Text("Acceleration: %.2f", acceleration);
                ImGui::Text("Gravity: %.2f", gravity);
                ImGui::Text("Controller Mode: %s", controllerMode);
            }
            ImGui::End();
        }

        void UIManager::ShowStaminaBar(float stamina, float maxStamina, bool isSprinting) {
            // Position the stamina bar at the bottom center of the screen
            ImGuiIO& io = ImGui::GetIO();
            float barWidth = 200.0f;
            float barHeight = 20.0f;
            float posX = (io.DisplaySize.x - barWidth) * 0.5f;
            float posY = io.DisplaySize.y - 100.0f;
            
            ImGui::SetNextWindowPos(ImVec2(posX, posY));
            ImGui::SetNextWindowSize(ImVec2(barWidth + 20.0f, barHeight + 30.0f));
            ImGui::SetNextWindowBgAlpha(0.0f);
            
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | 
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            
            if (ImGui::Begin("Stamina Bar", nullptr, window_flags)) {
                // Calculate stamina percentage
                float staminaPercent = stamina / maxStamina;
                
                // Determine color based on stamina level and sprint state
                ImVec4 barColor;
                if (isSprinting) {
                    barColor = ImVec4(1.0f, 0.6f, 0.0f, 1.0f); // Orange when sprinting
                } else if (staminaPercent < 0.25f) {
                    barColor = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red when low
                } else {
                    barColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f); // Green normally
                }
                
                // Draw background
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetCursorScreenPos();
                draw_list->AddRectFilled(p, ImVec2(p.x + barWidth, p.y + barHeight), 
                                        IM_COL32(50, 50, 50, 200));
                draw_list->AddRect(p, ImVec2(p.x + barWidth, p.y + barHeight), 
                                  IM_COL32(255, 255, 255, 150));
                
                // Draw stamina fill
                if (staminaPercent > 0.0f) {
                    draw_list->AddRectFilled(p, ImVec2(p.x + barWidth * staminaPercent, p.y + barHeight),
                                           ImGui::GetColorU32(barColor));
                }
                
                // Add text label
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + barHeight + 5.0f);
                ImGui::SetCursorPosX((barWidth - ImGui::CalcTextSize("Stamina").x) * 0.5f + 10.0f);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.7f), "Stamina");
            }
            ImGui::End();
        }

    } // namespace UI
} // namespace Luminumbra
