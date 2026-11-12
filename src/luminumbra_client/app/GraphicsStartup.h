#pragma once

#include "luminumbra_common/core/SystemConfig.h"
#include "ui/GraphicsSettingsState.h"

#include <string>
#include <utility>

namespace Luminumbra::Client {

// Select before renderer initialization; persistence remains gated on successful apply.
// Existing and recovered settings change only on an explicit detection request.
inline bool SelectStartupGraphicsPreset(luminumbra::core::SystemConfig& config,
                                        bool detect_requested,
                                        const luminumbra::core::GraphicsHardwareInputs& inputs,
                                        std::string& error) {
    if (!config.settings_load_report().needs_first_launch_detection && !detect_requested) {
        error.clear();
        return true;
    }
    GraphicsEdit edit;
    edit.kind = GraphicsEditKind::DetectApply;
    luminumbra::core::GraphicsSettings detected;
    if (!ReduceGraphicsSettings(config.user().graphics, edit, inputs, detected, error)) {
        return false;
    }
    config.user().graphics = std::move(detected);
    return true;
}

} // namespace Luminumbra::Client
