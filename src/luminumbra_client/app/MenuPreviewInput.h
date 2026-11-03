#pragma once

#include "app/UiInputRouter.h"

#include <string>

namespace Luminumbra::Client::App {
// Candidate identity and cursor state for the create-world turntable.
struct MenuPreviewState {
    std::string lastSig;
    bool dragging = false;
    double lastCursorX = 0.0, lastCursorY = 0.0;

    bool UpdateInputOwnership(const UiInputRouter& router) {
        if (router.Owner() == InputOwner::RmlUi)
            return true;
        dragging = false;
        return false;
    }
};
} // namespace Luminumbra::Client::App
