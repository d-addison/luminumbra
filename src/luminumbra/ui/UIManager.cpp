#include "luminumbra/ui/UIManager.h"
#include "luminumbra/world/World.h"
#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/rendering/CloudManager.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

// For std::snprintf
#include <cstdio>

namespace Luminumbra {
    namespace UI {

        UIManager::UIManager(GLFWwindow* window) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO(); (void)io;
            ImGui::StyleColorsDark();
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 130");

            // Add placeholder data for testing UI elements
            m_Photos.push_back({
                "res/textures/placeholder_photo1.png", "Grove-Strider", "Lumin", 
                50.f, "Standard Lens", "First sighting. Very shy."
            });
            m_Photos.push_back({
                "res/textures/placeholder_photo2.png", "Flutterwing", "Umbra", 
                15.f, "Macro Lens", "Caught it feeding on glowing moss."
            });

            m_Requests.push_back({
                "Kael", "Grove-Strider", "From at least 100m away.", "Telephoto Lens"
            });
            m_Requests.push_back({
                "Linnea", "Sun-petal Flower", "At its most open point.", "Upgraded Glimmer-stone Recipe"
            });
            m_Requests.push_back({
                "Roric", "Skitter-Sprite", "Show me its hoard!", "Expanded Pack"
            });
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

        void UIManager::ShowSplashScreen() {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Splash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("Luminumbra");
            ImGui::End();
        }

        void UIManager::ShowLoadingScreen() {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("Loading...");
            ImGui::End();
        }

        void UIManager::ShowMainMenu(const std::function<void()>& newGameCallback, const std::function<void()>& loadGameCallback, const std::function<void()>& quitCallback) {
            ImGui::Begin("Main Menu");
            if (ImGui::Button("New Game")) {
                newGameCallback();
            }
            if (ImGui::Button("Load Game")) { loadGameCallback(); }
            if (ImGui::Button("Quit")) {
                quitCallback();
            }
            ImGui::End();
        }

        void UIManager::ShowPauseMenu(
            const std::function<void()>& resumeCallback,
            const std::function<void()>& settingsCallback,
            const std::function<void()>& saveGameCallback,
            const std::function<void()>& quitCallback)
        {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Paused", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

            if (ImGui::Button("Resume", ImVec2(150, 0))) {
                resumeCallback();
            }
            if (ImGui::Button("Settings", ImVec2(150, 0))) {
                settingsCallback(); // We can wire this up later
            }
            if (ImGui::Button("Save Game", ImVec2(150, 0))) { saveGameCallback(); }
            if (ImGui::Button("Quit to Main Menu", ImVec2(150, 0))) {
                quitCallback();
            }

            ImGui::End();
        }

        void UIManager::ShowLoadGameWindow(
            const std::vector<std::string>& saveFiles,
            const std::function<void(const std::string&)>& loadCallback,
            const std::function<void()>& backCallback)
        {
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Load Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

            ImGui::Text("Select a save file to load:");
            ImGui::Separator();

            if (saveFiles.empty()) {
                ImGui::TextDisabled("No save files found.");
            } else {
                for (const auto& saveName : saveFiles) {
                    if (ImGui::Selectable(saveName.c_str())) {
                        loadCallback(saveName);
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Back", ImVec2(120, 0))) {
                backCallback();
            }
            ImGui::End();
        }

        bool UIManager::ShowNewGameWindow(bool& launchGame, char* saveName, int saveNameSize, char* seed, int seedSize) {
            bool window_is_open = true; // Create a local variable to track state

            ImGui::SetNextWindowSize(ImVec2(350, 200), ImGuiCond_FirstUseEver);
            // Pass the local variable to ImGui::Begin
            if (ImGui::Begin("New Game", &window_is_open)) {
                ImGui::InputText("Save Name", saveName, saveNameSize);
                ImGui::InputText("Seed (optional)", seed, seedSize);

                if (ImGui::Button("Launch")) {
                    launchGame = true;
                    window_is_open = false; // Close window on launch
                }
                ImGui::SameLine();
                if (ImGui::Button("Back")) {
                    window_is_open = false; // Close window on back
                }
            }
            ImGui::End();

            return window_is_open; // Return whether the window is still open
        }

        void UIManager::ShowSettingsWindow(bool& vsync, int& shadowQuality, int& textureFiltering, float& masterVolume, float& musicVolume, float& effectsVolume, float& mouseSensitivity, bool& invertY) {
            if (!m_ShowSettingsWindow) return;

            ImGui::SetNextWindowSize(ImVec2(450, 550), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings", &m_ShowSettingsWindow)) {
                if (ImGui::BeginTabBar("SettingsTabs")) {
                    if (ImGui::BeginTabItem("Graphics")) {
                        ImGui::Checkbox("V-Sync", &vsync);

                        const char* shadowLevels[] = { "Low", "Medium", "High" };
                        ImGui::Combo("Shadow Quality", &shadowQuality, shadowLevels, IM_ARRAYSIZE(shadowLevels));

                        const char* filterLevels[] = { "Bilinear", "Trilinear" };
                        ImGui::Combo("Texture Filtering", &textureFiltering, filterLevels, IM_ARRAYSIZE(filterLevels));
                        
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Audio")) {
                        ImGui::SliderFloat("Master Volume", &masterVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Music Volume", &musicVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Effects Volume", &effectsVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Controls")) {
                        ImGui::SliderFloat("Mouse Sensitivity", &mouseSensitivity, 0.01f, 1.0f, "%.2f");
                        ImGui::Checkbox("Invert Y-Axis", &invertY);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }

                ImGui::Separator();
                if (ImGui::Button("Close", ImVec2(120, 0))) {
                    m_ShowSettingsWindow = false;
                }
            }
            ImGui::End();
        }

        void UIManager::ShowCodex() {
            if (!m_ShowCodex) return;

            ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Codex - Field Journal", &m_ShowCodex)) {
                ImGui::Columns(2, "CodexColumns");
                ImGui::SetColumnWidth(0, 200);

                // Left column: Photo gallery
                ImGui::BeginChild("PhotoGallery");
                ImGui::Text("Photos");
                ImGui::Separator();
                for (int i = 0; i < m_Photos.size(); ++i) {
                    char label[128];
                    std::snprintf(label, sizeof(label), "Photo %d: %s", i + 1, m_Photos[i].subjectID.c_str());
                    if (ImGui::Selectable(label, m_SelectedPhoto == i)) {
                        m_SelectedPhoto = i;
                    }
                }
                ImGui::EndChild();

                ImGui::NextColumn();

                // Right column: Selected photo details
                ImGui::BeginChild("PhotoDetails");
                if (m_SelectedPhoto >= 0 && m_SelectedPhoto < m_Photos.size()) {
                    Photo& photo = m_Photos[m_SelectedPhoto];
                    ImGui::Text("Photo Details");
                    ImGui::Separator();
                    
                    // In a real implementation, we'd load and show the texture.
                    // For now, we'll just show the path.
                    ImGui::Text("Image: %s", photo.texturePath.c_str());
                    ImGui::Image((void*)(intptr_t)0, ImVec2(256, 144), ImVec2(0,0), ImVec2(1,1), ImVec4(0.5,0.5,0.5,1), ImVec4(0.8,0.8,0.8,1));


                    ImGui::Text("Subject: %s (%s)", photo.subjectID.c_str(), photo.subjectState.c_str());
                    ImGui::Text("Distance: %.1fm", photo.distanceToSubject);
                    ImGui::Text("Lens: %s", photo.lensUsed.c_str());

                    ImGui::Separator();
                    ImGui::Text("Field Notes:");
                    char notes_buffer[256];
                    strncpy(notes_buffer, photo.notes.c_str(), sizeof(notes_buffer));
                    notes_buffer[sizeof(notes_buffer) - 1] = 0;
                    if (ImGui::InputTextMultiline("##Notes", notes_buffer, sizeof(notes_buffer), ImVec2(-1.0f, 150.0f))) {
                        photo.notes = std::string(notes_buffer);
                    }
                } else {
                    ImGui::Text("Select a photo to view details.");
                }
                ImGui::EndChild();

                ImGui::Columns(1);
            }
            ImGui::End();
        }

        void UIManager::ShowRequestBoard() {
            if (!m_ShowRequestBoard) return;

            ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settlement Request Board", &m_ShowRequestBoard)) {
                for (const auto& request : m_Requests) {
                    if (ImGui::CollapsingHeader(request.subject.c_str())) {
                        ImGui::Text("From: %s", request.npcName.c_str());
                        ImGui::TextWrapped("Request: %s", request.condition.c_str());
                        ImGui::Text("Reward: %s", request.reward.c_str());
                        if (ImGui::Button("Pin Request")) {
                            // Logic to pin request would go here
                        }
                        ImGui::Separator();
                    }
                }
            }
            ImGui::End();
        }

        void UIManager::ShowDebugOverlay(const glm::vec3& playerPos, const glm::vec3& playerVel, float gravity, bool isNoClip, const World::World& world, const Rendering::Shader& worldShader, Core::PostProcessSettings& settings) {
            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            
            if (ImGui::Begin("Debug Info", nullptr, window_flags)) {
                ImGui::Text("Luminumbra Debug");
                ImGui::Separator();
                
                // Player Info
                ImGui::Text("Pos (XYZ): %.2f, %.2f, %.2f", playerPos.x, playerPos.y, playerPos.z);
                ImGui::Text("Vel (XYZ): %.2f, %.2f, %.2f", playerVel.x, playerVel.y, playerVel.z);
                ImGui::Text("Speed: %.2f m/s", glm::length(glm::vec2(playerVel.x, playerVel.z)));
                ImGui::Text("Mode: %s", isNoClip ? "NoClip" : "Normal");
                ImGui::Text("Gravity: %.2f", gravity);
                
                ImGui::Separator();
                
                // World Info
                ImGui::Text("Biome: %s", world.getBiome(playerPos));
                float time = world.getTimeOfDay();
                int hours = (int)(time * 24.0f) % 24;
                int minutes = (int)((time * 24.0f - hours) * 60.0f);
                ImGui::Text("Time: %02d:%02d (%.2f)", hours, minutes, time);
                
                ImGui::Separator();

                // Shader Info
                ImGui::Text("World Shader:");
                ImGui::Text("  Vert: %s", worldShader.getVertexPath().c_str());
                ImGui::Text("  Frag: %s", worldShader.getFragmentPath().c_str());

                ImGui::Separator();
                ImGui::Text("Auxiliary Shaders:");
                // const auto& skyboxShader = world.getSkybox().getShader();
                // ImGui::Text("  Skybox: %s, %s", skyboxShader.getVertexPath().c_str(), skyboxShader.getFragmentPath().c_str());
                const auto& cloudShader = world.getCloudManager().getShader();
                ImGui::Text("  Clouds: %s, %s", cloudShader.getVertexPath().c_str(), cloudShader.getFragmentPath().c_str());

                ImGui::Separator();

                // Settings
                ImGui::Text("F3: Toggle Debug | F4: Wireframe");
            }

            if (ImGui::CollapsingHeader("Post-Processing")) {
                // Enable/disable effects
                ImGui::Checkbox("Enable Bloom", &settings.enableBloom);
                ImGui::Checkbox("Enable Depth of Field", &settings.enableDof);
                ImGui::Checkbox("Enable God Rays", &settings.enableGodRays);
                
                ImGui::Separator();
                
                // Bloom settings
                if (settings.enableBloom) {
                    if (ImGui::TreeNode("Bloom Settings")) {
                        ImGui::SliderFloat("Threshold", &settings.bloomThreshold, 0.0f, 2.0f, "%.2f");
                        ImGui::SliderFloat("Intensity", &settings.bloomIntensity, 0.0f, 2.0f, "%.2f");
                        ImGui::TreePop();
                    }
                }
                
                // DoF settings
                if (settings.enableDof) {
                    if (ImGui::TreeNode("Depth of Field Settings")) {
                        ImGui::SliderFloat("Focal Distance", &settings.dofFocalDistance, 0.1f, 100.0f, "%.1f");
                        ImGui::SliderFloat("Focal Range", &settings.dofFocalRange, 0.1f, 50.0f, "%.1f");
                        ImGui::TreePop();
                    }
                }
                
                // God Rays settings
                if (settings.enableGodRays) {
                    if (ImGui::TreeNode("God Rays Settings")) {
                        ImGui::SliderFloat("Density", &settings.godRaysDensity, 0.0f, 1.0f, "%.2f");
                        ImGui::TreePop();
                    }
                }
                
                // Global post-processing settings
                if (ImGui::TreeNode("Global Settings")) {
                    ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 5.0f, "%.2f");
                    ImGui::TreePop();
                }
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

        void UIManager::ShowCameraViewfinder(const std::string& lens, float aperture, float shutterSpeed) {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::SetNextWindowBgAlpha(0.0f); // Transparent window

            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings;

            if (ImGui::Begin("Viewfinder", nullptr, flags)) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                // Simple crosshair
                float centerX = io.DisplaySize.x * 0.5f;
                float centerY = io.DisplaySize.y * 0.5f;
                draw_list->AddLine(ImVec2(centerX - 10, centerY), ImVec2(centerX + 10, centerY), IM_COL32(255, 255, 255, 150));
                draw_list->AddLine(ImVec2(centerX, centerY - 10), ImVec2(centerX, centerY + 10), IM_COL32(255, 255, 255, 150));

                // Camera settings text at the bottom
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "%s | f/%.1f | 1/%.0f", lens.c_str(), aperture, shutterSpeed);
                ImVec2 textSize = ImGui::CalcTextSize(buffer);
                ImGui::SetCursorPosX((io.DisplaySize.x - textSize.x) * 0.5f);
                ImGui::SetCursorPosY(io.DisplaySize.y - textSize.y - 20.0f);
                ImGui::Text("%s", buffer);
            }
            ImGui::End();
        }

        void UIManager::ShowInteractionPrompt(const std::string& prompt) {
            if (prompt.empty()) return;

            ImGuiIO& io = ImGui::GetIO();
            ImVec2 textSize = ImGui::CalcTextSize(prompt.c_str());
            ImVec2 windowPadding = ImVec2(20, 10);
            ImVec2 windowSize = ImVec2(textSize.x + windowPadding.x * 2, textSize.y + windowPadding.y * 2);
            
            ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - windowSize.x) * 0.5f, io.DisplaySize.y * 0.75f));
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

            if (ImGui::Begin("InteractionPrompt", nullptr, flags)) {
                ImGui::Text("%s", prompt.c_str());
            }
            ImGui::End();
        }

        void UIManager::ShowPostProcessingSettings(Core::PostProcessSettings& settings) {
            ImGui::Begin("Post-Processing");
            ImGui::SliderFloat("Bloom Threshold", &settings.bloomThreshold, 0.0f, 2.0f);
            ImGui::SliderFloat("Bloom Intensity", &settings.bloomIntensity, 0.0f, 2.0f);
            ImGui::SliderFloat("DoF Focal Distance", &settings.dofFocalDistance, 0.1f, 100.0f);
            ImGui::SliderFloat("DoF Focal Range", &settings.dofFocalRange, 0.1f, 50.0f);
            ImGui::SliderFloat("God Rays Density", &settings.godRaysDensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 5.0f);
            ImGui::End();
        }

    } // namespace UI
} // namespace Luminumbra
