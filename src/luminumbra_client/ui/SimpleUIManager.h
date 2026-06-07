#pragma once

#include "Rml_UIManager.h"
#include <string>

namespace Luminumbra::Client {

/**
 * Simple UI Manager - Minimal extension of RmlUi
 * Just adds a few convenience methods without breaking the build
 */
class SimpleUIManager : public Rml_UIManager {
public:
    explicit SimpleUIManager(const std::string& asset_root_path)
        : Rml_UIManager(asset_root_path) {}
    
    ~SimpleUIManager() = default;
    
    // Simple convenience methods
    void ShowMainMenu() {
        RequestLoadDocument("main_menu.rml");
    }
    
    void ShowWorldSelection() {
        RequestLoadDocument("world_selection.rml");
    }
    
    void ShowWorldCreation() {
        RequestLoadDocument("world_creation.rml");
    }
    
    void ShowLoading() {
        RequestLoadDocument("loading.rml");
    }
};

} // namespace Luminumbra::Client