#include "UIIntegration.h"
#include "components/game/WorldList.h"
#include "core/Log.h"
#include <RmlUi/Debugger.h>

namespace Luminumbra::Client::UI {

std::unique_ptr<UIManager> UIIntegration::s_uiManager = nullptr;

void UIIntegration::InitializeNewUISystem(const std::string& asset_root_path, GLFWwindow* window, IAudioManager* audioManager) {
    LUMINUMBRA_CORE_INFO("[UI Integration] Initializing new UI system...");
    
    // Create and initialize the new UI manager
    s_uiManager = std::make_unique<UIManager>(asset_root_path);
    s_uiManager->Init(window, audioManager);
    
    // Set up default bindings
    SetupDefaultBindings();
    
    // Load initial document
    s_uiManager->LoadDocument("main_menu.rml");
    
    LUMINUMBRA_CORE_INFO("[UI Integration] New UI system initialized successfully");
}

void UIIntegration::SetupDefaultBindings() {
    if (!s_uiManager) return;
    
    auto& state = GetUIState();
    
    // Example: Set up some initial world data for testing
    std::vector<WorldInfo> testWorlds = {
        {"world_001", "My First World", "default", "1337", "2024-08-24T10:30:00", 15728640},
        {"world_002", "Archipelago Adventure", "archipelago", "98765", "2024-08-20T15:45:00", 44728320},
        {"world_003", "Mountain Explorer", "mountains", "", "2024-08-18T09:20:00", 28934144}
    };
    
    state.worldList.Set(testWorlds);
    
    // Set up callbacks for world operations
    s_uiManager->SetWorldCreationCallback([](const std::string& name, const std::string& seed, const std::string& type) {
        LUMINUMBRA_CORE_INFO("[UI] Create world: {} (seed: {}, type: {})", name, seed, type);
        // TODO: Integrate with actual world creation system
        GetUIState().ShowNotification("World '" + name + "' created successfully!");
    });
    
    s_uiManager->SetLoadWorldCallback([](const std::string& worldId) {
        LUMINUMBRA_CORE_INFO("[UI] Load world: {}", worldId);
        // TODO: Integrate with actual world loading system
        GetUIState().ShowNotification("Loading world...");
    });
    
    LUMINUMBRA_CORE_INFO("[UI Integration] Default bindings set up");
}

void UIIntegration::EnableDevelopmentMode(bool enabled) {
    if (!s_uiManager) return;
    
    // Enable hot-reload for rapid development
    s_uiManager->EnableHotReload(enabled);
    
    if (enabled) {
        // Enable debugger
        ShowUIDebugger(true);
        
        // Show development notifications
        GetUIState().ShowNotification("Development mode enabled - F8 to toggle debugger", 5.0f);
        
        LUMINUMBRA_CORE_INFO("[UI Integration] Development mode enabled");
    } else {
        ShowUIDebugger(false);
        LUMINUMBRA_CORE_INFO("[UI Integration] Development mode disabled");
    }
}

UIManager* UIIntegration::GetUIManager() {
    return s_uiManager.get();
}

UIStateManager* UIIntegration::GetStateManager() {
    return &GetUIState();
}

// Global convenience functions
UIStateManager& GetUIState() {
    return UIStateManager::Instance();
}

UIManager& GetUIManager() {
    if (!UIIntegration::GetUIManager()) {
        throw std::runtime_error("UI Manager not initialized. Call UIIntegration::InitializeNewUISystem first.");
    }
    return *UIIntegration::GetUIManager();
}

void EnableUIHotReload(bool enabled) {
    if (auto* manager = UIIntegration::GetUIManager()) {
        manager->EnableHotReload(enabled);
    }
}

void ReloadUITheme() {
    if (auto* manager = UIIntegration::GetUIManager()) {
        manager->ReloadCurrentTheme();
    }
}

void ShowUIDebugger(bool show) {
    Rml::Debugger::SetVisible(show);
}

} // namespace Luminumbra::Client::UI