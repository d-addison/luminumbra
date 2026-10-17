#pragma once

#include <optional>
#include <string>

namespace Luminumbra::Client::Test {

// Minimal RmlUi element seam, including attributes whose controls remain captured.
struct FocusedUiElement {
    std::string tag;
    const FocusedUiElement* parent = nullptr;
    std::optional<std::string> contentEditable = std::nullopt;
    bool readOnly = false;
    std::optional<std::string> type = std::nullopt;

    const std::string& GetTagName() const {
        return tag;
    }
    const FocusedUiElement* GetParentNode() const {
        return parent;
    }
    bool HasAttribute(const std::string& name) const {
        return (name == "contenteditable" && contentEditable.has_value()) ||
               (name == "readonly" && readOnly) || (name == "type" && type.has_value());
    }
    template<typename T>
    T GetAttribute(const std::string& name, const T& fallback) const {
        if (name == "contenteditable" && contentEditable)
            return T(*contentEditable);
        if (name == "readonly" && readOnly)
            return T("");
        if (name == "type" && type)
            return T(*type);
        return fallback;
    }
};

} // namespace Luminumbra::Client::Test
