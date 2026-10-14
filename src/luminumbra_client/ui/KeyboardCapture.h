#pragma once

#include <string>

namespace Luminumbra::Client {

// Use the RmlUi element API, but keep the policy independent of a renderer so it can
// also be exercised headless. Document/body/context focus alone does not capture keys.
template<typename Element>
bool FocusedElementCapturesKeyboard(const Element* element) {
    bool editableSpecified = false;
    for (; element; element = element->GetParentNode()) {
        const auto& tag = element->GetTagName();
        if (tag == "input" || tag == "textarea" || tag == "select")
            return true;
        // Editable descendants inherit the nearest contenteditable value.
        if (!editableSpecified && element->HasAttribute("contenteditable")) {
            editableSpecified = true;
            if (element->template GetAttribute<std::string>("contenteditable", "") != "false")
                return true;
        }
    }
    return false;
}

} // namespace Luminumbra::Client
