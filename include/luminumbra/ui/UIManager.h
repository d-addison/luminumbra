#pragma once

#include <functional>
#include <string>
#include <vector>

// Forward declarations to reduce header includes
struct GLFWwindow;

namespace Luminumbra::Player { class Player; }
namespace Luminumbra::World { class World; }
namespace Luminumbra::Core {
    struct PostProcessSettings;
    struct GameSettings;
}

namespace Luminumbra {
    namespace UI {
        struct Photo {
            std::string texturePath;
            std::string subjectID;
            std::string subjectState;
            float distanceToSubject;
            std::string lensUsed;
            std::string notes;
        };

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

            void ShowSplashScreen();
            void ShowLoadingScreen();
            void ShowMainMenu(const std::function<void()>& newGameCallback,
                              const std::function<void()>& loadGameCallback,
                              const std::function<void()>& settingsCallback,
                              const std::function<void()>& quitCallback);
            void ShowPauseMenu(const std::function<void()>& resumeCallback,
                               const std::function<void()>& settingsCallback,
                               const std::function<void()>& saveGameCallback,
                               const std::function<void()>& quitCallback);
            void ShowLoadGameWindow(const std::vector<std::string>& saveFiles,
                                    const std::function<void(const std::string&)>& loadCallback,
                                    const std::function<void()>& backCallback);
            bool ShowNewGameWindow(bool& launchGame, char* saveName, int saveNameSize, char* seed, int seedSize);
            void ShowSettingsWindow(bool& show, Core::GameSettings& settings, Core::PostProcessSettings& ppSettings);
            void ShowDebugOverlay(float deltaTime, const Player::Player& player, World::World& world, Core::PostProcessSettings& settings);
            void ShowStaminaBar(float stamina, float maxStamina, bool isSprinting);
            void ShowCameraViewfinder(const std::string& lens, float aperture, float shutterSpeed);
            void ShowInteractionPrompt(const std::string& prompt);

            // Toggles for UI windows
            void ToggleCodex() { m_ShowCodex = !m_ShowCodex; }
            void ToggleRequestBoard() { m_ShowRequestBoard = !m_ShowRequestBoard; }

        private:
            void ShowPostProcessingSettings(Core::PostProcessSettings& settings);
            void ShowCodex();
            void ShowRequestBoard();

            // UI State
            bool m_ShowSettingsWindow = false;
            bool m_ShowCodex = false;
            bool m_ShowRequestBoard = false;
            int m_SelectedPhoto = -1;

            // Music flags
            int m_SplashScreenMusicID = 0;
            int m_MainMenuMusicID = 0;

            // Placeholder data
            std::vector<Photo> m_Photos;
            std::vector<Request> m_Requests;
        };
    } // namespace UI
} // namespace Luminumbra