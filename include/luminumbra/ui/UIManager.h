#pragma once

struct GLFWwindow;

namespace Luminumbra {
    namespace UI {

        class UIManager {
        public:
            UIManager(GLFWwindow* window);
            ~UIManager();

            void NewFrame();
            void Render();

            void ShowMainMenu();
            void ShowSettingsWindow();
            void ShowDebugOverlay(double x, double y, double z, double acceleration, double gravity, const char* controllerMode);
            void ShowStaminaBar(float stamina, float maxStamina, bool isSprinting);

        private:
            bool m_ShowMainMenu = true;
            bool m_ShowSettingsWindow = false;
        };

    } // namespace UI
} // namespace Luminumbra
