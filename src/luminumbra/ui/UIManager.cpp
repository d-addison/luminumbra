#include "luminumbra/ui/UIManager.h"
#include "luminumbra/world/World.h"
#include "luminumbra/rendering/Skybox.h"
#include "luminumbra/rendering/CloudManager.h"
#include "luminumbra/audio/AudioManager.h"
#include "luminumbra/core/GameSettings.h"
#include "luminumbra/core/PostProcessSettings.h"
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
            if (m_SplashScreenMusicID == 0) {
                Audio::AudioManager::getInstance().playMusic(Audio::SoundEvent::MusicSplashScreen);
                m_SplashScreenMusicID = 1;
            }
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

        void UIManager::ShowMainMenu(const std::function<void()>& newGameCallback,
                                     const std::function<void()>& loadGameCallback,
                                     const std::function<void()>& settingsCallback,
                                     const std::function<void()>& quitCallback) {
            // Stop splash music when the main menu appears
            if (m_SplashScreenMusicID != 0) {
                // Stop the music and reset the flag
                Audio::AudioManager::getInstance().stopMusic();
                m_SplashScreenMusicID = 0;
            }
            // Start main menu music
            if (m_MainMenuMusicID == 0) {
                // Use playMusic here as well
                Audio::AudioManager::getInstance().playMusic(Audio::SoundEvent::MusicMainMenu);
                m_MainMenuMusicID = 1; // Use a simple flag
            }

            ImGui::Begin("Main Menu");
            if (ImGui::Button("New Game")) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                newGameCallback();
            }
            if (ImGui::Button("Load Game")) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                loadGameCallback();
            }
            if (ImGui::Button("Settings")) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                settingsCallback();
            }
            if (ImGui::Button("Quit")) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
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
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                resumeCallback();
            }
            if (ImGui::Button("Settings", ImVec2(150, 0))) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                settingsCallback();
            }
            if (ImGui::Button("Save Game", ImVec2(150, 0))) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                saveGameCallback();
            }
            if (ImGui::Button("Quit to Main Menu", ImVec2(150, 0))) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
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
                        Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                        loadCallback(saveName);
                    }
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Back", ImVec2(120, 0))) {
                Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
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
                if (ImGui::IsItemDeactivatedAfterEdit()) { Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick); }

                ImGui::InputText("Seed (optional)", seed, seedSize);
                if (ImGui::IsItemDeactivatedAfterEdit()) { Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick); }

                if (ImGui::Button("Launch")) {
                    Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                    launchGame = true;
                    window_is_open = false; // Close window on launch
                }
                ImGui::SameLine();
                if (ImGui::Button("Back")) {
                    Audio::AudioManager::getInstance().playSound(Audio::SoundEvent::UIClick);
                    window_is_open = false; // Close window on back
                }
            }
            ImGui::End();

            return window_is_open; // Return whether the window is still open
        }

        void UIManager::ShowSettingsWindow(bool& showWindow, Core::GameSettings& settings, Core::PostProcessSettings& ppSettings) {
            if (!showWindow) return;

            ImGui::SetNextWindowSize(ImVec2(550, 450), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings", &showWindow)) {
                if (ImGui::BeginTabBar("SettingsTabs")) {
                    // --- GRAPHICS TAB ---
                    if (ImGui::BeginTabItem("Graphics")) {
                        ImGui::Text("Display");
                        ImGui::Separator();
                        ImGui::Checkbox("V-Sync", &settings.vsync);

                        ImGui::Dummy(ImVec2(0.0f, 15.0f));
                        ImGui::Text("Quality");
                        ImGui::Separator();
                        const char* shadowLevels[] = { "Low", "Medium", "High", "Ultra" };
                        ImGui::Combo("Shadow Quality", &settings.shadowQuality, shadowLevels, IM_ARRAYSIZE(shadowLevels));

                        const char* filterLevels[] = { "Bilinear", "Trilinear" };
                        ImGui::Combo("Texture Filtering", &settings.textureFiltering, filterLevels, IM_ARRAYSIZE(filterLevels));

                        ImGui::Dummy(ImVec2(0.0f, 15.0f));
                        ImGui::Text("Post-Processing");
                        ImGui::Separator();
                        ImGui::Checkbox("Enable Bloom", &ppSettings.enableBloom);
                        ImGui::Checkbox("Enable Depth of Field", &ppSettings.enableDof);
                        ImGui::Checkbox("Enable God Rays", &ppSettings.enableGodRays);
                        
                        ImGui::Dummy(ImVec2(0.0f, 15.0f));
                        ImGui::Text("Tone Mapping");
                        ImGui::Separator();
                        ImGui::SliderFloat("Exposure", &ppSettings.exposure, 0.1f, 5.0f);

                        ImGui::EndTabItem();
                    }

                    // --- AUDIO TAB ---
                    if (ImGui::BeginTabItem("Audio")) {
                        ImGui::SliderFloat("Master Volume", &settings.masterVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Music Volume", &settings.musicVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Effects Volume", &settings.effectsVolume, 0.0f, 1.0f, "%.2f");
                        ImGui::EndTabItem();
                    }

                    // --- CONTROLS TAB ---
                    if (ImGui::BeginTabItem("Controls")) {
                        ImGui::SliderFloat("Mouse Sensitivity", &settings.mouseSensitivity, 0.01f, 1.0f, "%.3f");
                        ImGui::Checkbox("Invert Y-Axis", &settings.invertY);
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::Separator();
                if (ImGui::Button("Close", ImVec2(120, 0))) {
                    showWindow = false;
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

        void UIManager::ShowDebugOverlay(float deltaTime, const Player::Player& player, World::World& world, Core::PostProcessSettings& settings) {
            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowBgAlpha(0.65f);
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            
            if (ImGui::Begin("Debug Info", nullptr, window_flags)) {
                ImGui::Text("Luminumbra Debug Panel (F3 to hide)");
                ImGui::Separator();
                
                // Performance
                ImGui::Text("FPS: %.1f (%.3f ms)", 1.0f / deltaTime, deltaTime * 1000.0f);
                
                // Player Info
                if (ImGui::CollapsingHeader("Player State")) {
                    const auto& pos = player.getPosition();
                    const auto& vel = player.getVelocity();
                    ImGui::Text("Pos: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
                    ImGui::Text("Vel: %.2f, %.2f, %.2f | Speed: %.2f", vel.x, vel.y, vel.z, glm::length(glm::vec2(vel.x, vel.z)));
                    ImGui::Text("Stamina: %.1f / %.1f", player.getStamina(), player.getMaxStamina());
                    ImGui::Text("Fall Distance: %.2f", player.getFallDistance());
                    ImGui::Text("OnGround: %s, Sprint: %s, Crouch: %s", player.isSliding() ? "Yes" : "No", player.isSprinting() ? "Yes" : "No", player.isCrouching() ? "Yes" : "No");
                    ImGui::Text("Glide: %s, Climb: %s, Slide: %s", player.isGliding() ? "Yes" : "No", player.isClimbing() ? "Yes" : "No", player.isSliding() ? "Yes" : "No");
                }

                // World Info
                if (ImGui::CollapsingHeader("World State")) {
                    ImGui::Text("Loaded Chunks: %zu", world.getChunks().size());
                    ImGui::Text("Active Particles: %u", world.getParticleSystem()->getActiveParticleCount());
                    float time = world.getTimeOfDay();
                    if (ImGui::SliderFloat("Time of Day", &time, 0.0f, 1.0f)) {
                        world.setTimeOfDay(time);
                    }

                    // Weather Controls
                    auto weatherManager = world.getWeatherManager();
                    const char* weatherNames[] = { "Clear", "Rain", "Thunderstorm", "Snow" };
                    int currentItem = static_cast<int>(weatherManager->getCurrentWeatherType());
                    if (ImGui::Combo("Weather", &currentItem, weatherNames, IM_ARRAYSIZE(weatherNames))) {
                        world.setWeather(static_cast<World::WeatherType>(currentItem));
                    }
                    ImGui::Text("Weather Intensity: %.2f", weatherManager->getIntensity());
                    ImGui::Text("Wetness: %.2f", weatherManager->getWetness());
                }

                // Camera Info
                if (ImGui::CollapsingHeader("Camera State")) {
                    const auto& camera = player.getCamera();
                    const auto& pos = camera.getPosition();
                    const auto& front = camera.getFront();
                    ImGui::Text("Cam Pos: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
                    ImGui::Text("Cam Front: %.2f, %.2f, %.2f", front.x, front.y, front.z);
                    ImGui::Text("Pitch: %.2f | Yaw: (Implicit)", camera.getPitch());
                    ImGui::Text("FOV: %.1f", camera.getFov());
                }

                if (ImGui::CollapsingHeader("World Generation")) {
                    auto& profile = world.getGenerationProfile();

                    ImGui::Text("Global Settings");
                    ImGui::SliderFloat("Water Level", &profile.water_level, 0.0f, 50.0f);
                    ImGui::SliderFloat("Island Fade End", &profile.island_fade_end, 5.0f, 100.0f);
                    
                    ImGui::Separator();
                    
                    if (ImGui::TreeNode("Whispering Glade Profile")) {
                        auto& biome = profile.biome_profiles.at(World::BiomeType::WHISPERING_GLADE);
                        ImGui::SliderFloat("Base Height##WG", &biome.base_height, -50.0f, 50.0f);
                        ImGui::SliderFloat("Variance##WG", &biome.terrain_variance, 5.0f, 100.0f);
                        ImGui::SliderFloat("Mountain Amp##WG", &biome.mountains.amplitude, 0.0f, 500.0f);
                        ImGui::SliderFloat("Cave Amp##WG", &biome.caves.amplitude, 0.0f, 50.0f);
                        ImGui::SliderFloat("Base Freq##WG", &biome.base_terrain.noise.frequency, 0.001f, 0.02f);
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode("Crystal Groves Profile")) {
                        auto& biome = profile.biome_profiles.at(World::BiomeType::CRYSTAL_GROVES);
                        ImGui::SliderFloat("Base Height##CG", &biome.base_height, -50.0f, 50.0f);
                        ImGui::SliderFloat("Variance##CG", &biome.terrain_variance, 5.0f, 100.0f);
                        ImGui::SliderFloat("Mountain Amp##CG", &biome.mountains.amplitude, 0.0f, 500.0f);
                        ImGui::SliderFloat("Cave Amp##CG", &biome.caves.amplitude, 0.0f, 50.0f);
                        ImGui::SliderFloat("Base Freq##CG", &biome.base_terrain.noise.frequency, 0.001f, 0.02f);
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode("Sunken Hollows Profile")) {
                        auto& biome = profile.biome_profiles.at(World::BiomeType::SUNKEN_HOLLOWS);
                        ImGui::SliderFloat("Base Height##SH", &biome.base_height, -50.0f, 50.0f);
                        ImGui::SliderFloat("Variance##SH", &biome.terrain_variance, 5.0f, 100.0f);
                        ImGui::SliderFloat("Mountain Amp##SH", &biome.mountains.amplitude, 0.0f, 500.0f);
                        ImGui::SliderFloat("Cave Amp##SH", &biome.caves.amplitude, 0.0f, 50.0f);
                        ImGui::SliderFloat("Base Freq##SH", &biome.base_terrain.noise.frequency, 0.001f, 0.02f);
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode("Canopy Bridges Profile")) {
                        auto& biome = profile.biome_profiles.at(World::BiomeType::CANOPY_BRIDGES);
                        ImGui::SliderFloat("Base Height##CB", &biome.base_height, -50.0f, 50.0f);
                        ImGui::SliderFloat("Variance##CB", &biome.terrain_variance, 5.0f, 100.0f);
                        ImGui::SliderFloat("Mountain Amp##CB", &biome.mountains.amplitude, 0.0f, 500.0f);
                        ImGui::SliderFloat("Cave Amp##CB", &biome.caves.amplitude, 0.0f, 50.0f);
                        ImGui::SliderFloat("Base Freq##CB", &biome.base_terrain.noise.frequency, 0.001f, 0.02f);
                        ImGui::TreePop();
                    }

                    if (ImGui::TreeNode("Sky Void Profile")) {
                        auto& biome = profile.biome_profiles.at(World::BiomeType::SKY_VOID);
                        ImGui::SliderFloat("Base Height##SV", &biome.base_height, -50.0f, 50.0f);
                        ImGui::SliderFloat("Variance##SV", &biome.terrain_variance, 5.0f, 100.0f);
                        ImGui::SliderFloat("Mountain Amp##SV", &biome.mountains.amplitude, 0.0f, 500.0f);
                        ImGui::SliderFloat("Cave Amp##SV", &biome.caves.amplitude, 0.0f, 50.0f);
                        ImGui::SliderFloat("Base Freq##SV", &biome.base_terrain.noise.frequency, 0.001f, 0.02f);
                        ImGui::TreePop();
                    }

                    if (ImGui::Button("APPLY & REGENERATE WORLD")) {
                        // This function needs to be implemented in World.cpp
                        // It should clear all chunks and re-queue those around the player.
                        profile.applyChanges(world);
                    }
                }

                // Post-Processing
                if (ImGui::CollapsingHeader("Post-Processing")) {
                    ShowPostProcessingSettings(settings);
                }

                // Debug Actions
                if (ImGui::CollapsingHeader("Actions")) {
                    if (ImGui::Button("Start Fire")) { world.startFireNearPlayer(); }
                    if (ImGui::TreeNode("Teleport")) {
                        if (ImGui::Button("To Origin Spawn")) { world.teleportToEffect("spawn"); }
                        if (ImGui::Button("To Campfire")) { world.teleportToEffect("campfire"); }
                        if (ImGui::Button("To Portal")) { world.teleportToEffect("portal"); }
                        ImGui::TreePop();
                    }
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
            // This is now just for the debug menu, so it doesn't need a Begin/End block.
            ImGui::Checkbox("Enable Bloom", &settings.enableBloom);
            ImGui::SliderFloat("Bloom Threshold", &settings.bloomThreshold, 0.0f, 2.0f);
            ImGui::SliderFloat("Bloom Intensity", &settings.bloomIntensity, 0.0f, 2.0f);
            ImGui::Separator();
            ImGui::Checkbox("Enable DoF", &settings.enableDof);
            ImGui::SliderFloat("DoF Focal Distance", &settings.dofFocalDistance, 0.1f, 100.0f);
            ImGui::SliderFloat("DoF Focal Range", &settings.dofFocalRange, 0.1f, 50.0f);
            ImGui::Separator();
            ImGui::Checkbox("Enable God Rays", &settings.enableGodRays);
            ImGui::SliderFloat("God Rays Density", &settings.godRaysDensity, 0.0f, 1.0f);
            ImGui::Separator();
            ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 5.0f);
        }

    } // namespace UI
} // namespace Luminumbra