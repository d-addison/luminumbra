#include "UIStateManager.h"
#include "core/Log.h"

namespace Luminumbra::Client::UI {

void UIStateManager::SetPlayerHealth(int health, int maxHealth) {
    playerHealth.Set(health);
    playerMaxHealth.Set(maxHealth);
    
    if (maxHealth > 0) {
        playerHealthPercent.Set(static_cast<float>(health) / static_cast<float>(maxHealth));
    } else {
        playerHealthPercent.Set(0.0f);
    }
}

void UIStateManager::ShowNotification(const std::string& message, float timeout) {
    notificationMessage.Set(message);
    notificationTimeout.Set(timeout);
    LUMINUMBRA_CORE_INFO("[UI] Notification: {}", message);
}

void UIStateManager::ClearNotification() {
    notificationMessage.Set("");
    notificationTimeout.Set(0.0f);
}

void UIStateManager::NavigateToDocument(const std::string& document) {
    LUMINUMBRA_CORE_INFO("[UI] Navigating to document: {}", document);
    currentUIDocument.Set(document);
    
    // Update game state based on document
    if (document == "main_menu.rml") {
        currentGameState.Set(GameState::MainMenu);
        isMainMenu.Set(true);
        isInGame.Set(false);
    } else if (document == "world_creation.rml") {
        currentGameState.Set(GameState::WorldCreation);
        isMainMenu.Set(false);
    } else if (document == "world_selection.rml") {
        currentGameState.Set(GameState::WorldSelection);
        isMainMenu.Set(false);
    }
}

void UIStateManager::ShowModal(const std::string& modalId) {
    LUMINUMBRA_CORE_INFO("[UI] Showing modal: {}", modalId);
    activeModal.Set(modalId);
}

void UIStateManager::HideModal() {
    LUMINUMBRA_CORE_INFO("[UI] Hiding modal");
    activeModal.Set("");
}

} // namespace Luminumbra::Client::UI