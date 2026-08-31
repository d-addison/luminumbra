#pragma once

#include "UIProperty.h"
#include <cstddef>
#include <string>
#include <vector>

namespace Luminumbra::Client::UI {

// Data structures for UI state
struct WorldInfo {
    std::string id;
    std::string name;
    std::string type;
    std::string seed;
    std::string lastPlayed;
    std::string createdAt;
    std::size_t fileSize = 0;
    bool favorite = false;

    bool operator==(const WorldInfo& other) const {
        return id == other.id;
    }

    bool operator!=(const WorldInfo& other) const {
        return !(*this == other);
    }
};

enum class GameState {
    MainMenu,
    InGame,
    Loading,
    Settings,
    WorldSelection,
    WorldCreation,
    Paused
};

/**
 * Centralized state management for the UI system.
 * Contains all observable properties that UI components can bind to.
 */
class UIStateManager {
public:
    static UIStateManager& Instance() {
        static UIStateManager instance;
        return instance;
    }

    // Player-related properties
    Property<std::string> playerName{"Player"};
    Property<int> playerHealth{100};
    Property<int> playerMaxHealth{100};
    Property<float> playerHealthPercent{1.0f};

    // Game state properties
    Property<GameState> currentGameState;
    Property<bool> isPaused{false};
    Property<bool> isInGame{false};
    Property<bool> isMainMenu{true};

    // World management properties
    Property<std::vector<WorldInfo>> worldList;
    Property<std::string> selectedWorldId;
    Property<bool> isLoadingWorld{false};
    Property<float> worldLoadingProgress{0.0f};

    // Settings properties
    Property<float> masterVolume{1.0f};
    Property<float> sfxVolume{1.0f};
    Property<float> musicVolume{1.0f};
    Property<int> renderDistance{8};
    Property<bool> vsyncEnabled{true};
    Property<bool> fullscreenEnabled{false};

    // UI state properties
    Property<std::string> currentUIDocument{"main_menu.rml"};
    Property<std::string> activeModal;
    Property<bool> showDebugInfo{false};
    Property<std::string> notificationMessage;
    Property<float> notificationTimeout{0.0f};

    // Methods for complex state updates
    void SetPlayerHealth(int health, int maxHealth);
    void ShowNotification(const std::string& message, float timeout = 3.0f);
    void ClearNotification();
    void NavigateToDocument(const std::string& document);
    void ShowModal(const std::string& modalId);
    void HideModal();

private:
    UIStateManager() = default;
    ~UIStateManager() = default;

    // Prevent copying
    UIStateManager(const UIStateManager&) = delete;
    UIStateManager& operator=(const UIStateManager&) = delete;
};

// Convenience function for accessing the state manager
inline UIStateManager& UIState() {
    return UIStateManager::Instance();
}

} // namespace Luminumbra::Client::UI
