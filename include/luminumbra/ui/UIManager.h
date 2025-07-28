#pragma once

#include "luminumbra/rendering/Shader.h"
#include "luminumbra/world/World.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>
#include <functional>
#include "luminumbra/core/PostProcessSettings.h" 
#include "luminumbra/audio/AudioManager.h"

struct GLFWwindow;

namespace Luminumbra {
    namespace UI {

        // Represents a photograph with its associated metadata (README 2.3.3)
        struct Photo {
            std::string texturePath;      // Path to the image file for rendering
            std::string subjectID;        // e.g., "Grove-Strider"
            std::string subjectState;     // e.g., "Transforming"
            float distanceToSubject;
            std::string lensUsed;
            std::string notes;            // Player-added notes
        };

        // Represents a request posted by an NPC (README 2.3.3)
        struct Request {
            std::string npcName;
            std::string subject;
            std::string condition;
            std::string reward;
        };

        class UIManager {
        public:
            UIManager(GLFWwindow* window);
            ~UIManager();

            void NewFrame();
            void Render();

            // Main UI Windows
            void ShowSplashScreen();
            void ShowLoadingScreen();
            void ShowPauseMenu(
                const std::function<void()>& resumeCallback,
                const std::function<void()>& settingsCallback,
                const std::function<void()>& saveGameCallback,
                const std::function<void()>& quitCallback
            );
            void ShowLoadGameWindow(
                const std::vector<std::string>& saveFiles,
                const std::function<void(const std::string&)>& loadCallback,
                const std::function<void()>& backCallback
            );
            void ShowMainMenu(
                const std::function<void()>& newGameCallback, 
                const std::function<void()>& loadGameCallback,
                const std::function<void()>& quitCallback
            );
            bool ShowNewGameWindow(bool& launchGame, char* saveName, int saveNameSize, char* seed, int seedSize);
            void ShowSettingsWindow(bool& vsync, int& shadowQuality, int& textureFiltering, float& masterVolume, float& musicVolume, float& effectsVolume, float& mouseSensitivity, bool& invertY);
            void ShowCodex();
            void ShowRequestBoard();

            // HUD / Overlays
            void ShowStaminaBar(float stamina, float maxStamina, bool isSprinting);
            void ShowCameraViewfinder(const std::string& lens, float aperture, float shutterSpeed);
            void ShowInteractionPrompt(const std::string& prompt);
            void ShowDebugOverlay(
                const glm::vec3& playerPos,
                const glm::vec3& playerVel,
                float gravity,
                bool isNoClip,
                const World::World& world,
                const Rendering::Shader& worldShader,
                Core::PostProcessSettings& settings
            );

            // State mutators
            void ToggleCodex() { m_ShowCodex = !m_ShowCodex; }
            void ToggleRequestBoard() { m_ShowRequestBoard = !m_ShowRequestBoard; }

        private:
            // Window visibility state
            // bool m_ShowMainMenu = true;
            // bool m_ShowNewGameWindow = false;
            bool m_ShowSettingsWindow = false;
            bool m_ShowCodex = false;
            bool m_ShowRequestBoard = false;

            // Placeholder data
            std::vector<Photo> m_Photos;
            std::vector<Request> m_Requests;
            int m_SelectedPhoto = -1;

            void ShowPostProcessingSettings(Core::PostProcessSettings& settings);

            uint32_t m_SplashScreenMusicID = 0;
            uint32_t m_MainMenuMusicID = 0;
        };

    } // namespace UI
} // namespace Luminumbra
