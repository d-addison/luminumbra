#pragma once

#include "SimpleUIManager.h"
#include <string>

namespace Luminumbra::Client {

/**
 * Enhanced UI Manager - Alias for SimpleUIManager for now
 * This maintains compatibility with existing code
 */
class EnhancedUIManager : public SimpleUIManager {
public:
    explicit EnhancedUIManager(const std::string& asset_root_path)
        : SimpleUIManager(asset_root_path) {}
    
    ~EnhancedUIManager() = default;
    
    // Compatibility methods that do nothing for now
    void EnableEnhancedFeatures(bool enabled = true) {}
    void EnableHotReload(bool enabled = true) {}
    void SetUseQuantumUI(bool use_modern) {}
    bool IsUsingQuantumUI() const { return false; }
    void ShowNotification(const std::string& message, float timeout = 3.0f) {}
    void ShowDebugWindows(bool show = true) {}
    void ShowPerformanceOverlay(bool show = true) {}
};

} // namespace Luminumbra::Client