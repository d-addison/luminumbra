#pragma once

#include "core/UIManager.h"
#include "core/UIStateManager.h"

namespace Luminumbra::Client::UI {

/**
 * Integration helper for the new UI system
 * Provides easy migration from the old Rml_UIManager to the new UIManager
 */
class UIIntegration {
public:
    static void InitializeNewUISystem(const std::string& asset_root_path, GLFWwindow* window, IAudioManager* audioManager);
    static void SetupDefaultBindings();
    static void EnableDevelopmentMode(bool enabled = true);
    static UIManager* GetUIManager();
    static UIStateManager* GetStateManager();
    
private:
    static std::unique_ptr<UIManager> s_uiManager;
};

// Global convenience functions for easy access
UIStateManager& GetUIState();
UIManager& GetUIManager();

// Development helpers
void EnableUIHotReload(bool enabled = true);
void ReloadUITheme();
void ShowUIDebugger(bool show = true);

} // namespace Luminumbra::Client::UI