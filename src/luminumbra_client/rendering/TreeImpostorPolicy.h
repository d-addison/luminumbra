#pragma once

#include <optional>
#include <string>

namespace Luminumbra::Rendering {

// Single source of truth for the LUMIN_TREE_IMPOSTORS environment switch, shared by
// RenderPipeline (whether the atlas is baked) and the world-entry content gate (whether
// a missing atlas refuses entry). Impostors are ON unless the variable is set to an
// empty value or a value beginning with '0' (documented as LUMIN_TREE_IMPOSTORS=0).
inline bool TreeImpostorsRequested(const std::optional<std::string>& value) {
    if (!value) {
        return true;
    }
    return !value->empty() && value->front() != '0';
}

} // namespace Luminumbra::Rendering
