#include "Button.h"
#include "core/Log.h"
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

namespace Luminumbra::Client::UI {

Button::Button(const std::string& elementId)
    : UIComponent(elementId) {}

void Button::OnElementSet() {
    if (!m_element)
        return;

    // Set initial styles
    UpdateStyles();
    UpdateClasses();

    // Set up event handlers
    OnClick([this](Rml::Event& event) { HandleClick(event); });
    OnHover([this](bool hovering) { HandleHover(hovering); });

    // Set initial text if provided
    if (!m_text.empty()) {
        SetText(m_text);
    }
}

void Button::SetText(const std::string& text) {
    m_text = text;
    if (m_element) {
        const std::string safeText = Rml::StringUtilities::EncodeRml(text);
        if (m_iconPath.empty()) {
            m_element->SetInnerRML(safeText);
        } else {
            const std::string safeIcon = Rml::StringUtilities::EncodeRml(m_iconPath);
            std::string content = "<img src=\"" + safeIcon + "\" /> " + safeText;
            m_element->SetInnerRML(content);
        }
    }
}

void Button::SetStyle(Style style) {
    if (m_style != style) {
        m_style = style;
        UpdateClasses();
    }
}

void Button::SetSize(Size size) {
    if (m_size != size) {
        m_size = size;
        UpdateClasses();
    }
}

void Button::SetEnabled(bool enabled) {
    if (m_enabled != enabled) {
        m_enabled = enabled;
        if (m_element) {
            if (enabled) {
                RemoveClass("disabled");
                m_element->RemoveAttribute("disabled");
            } else {
                AddClass("disabled");
                m_element->SetAttribute("disabled", "");
            }
        }
    }
}

void Button::SetIcon(const std::string& iconPath) {
    m_iconPath = iconPath;
    // Refresh text to include icon
    SetText(m_text);
}

void Button::SetClickHandler(ClickHandler handler) {
    m_clickHandler = std::move(handler);
}

void Button::BindEnabled(Property<bool>& property) {
    // Set initial state
    SetEnabled(property.Get());

    // Subscribe to changes
    TrackSubscription(property,
                      [this](const bool& oldValue, const bool& newValue) { SetEnabled(newValue); });
}

void Button::BindText(Property<std::string>& property) {
    // Set initial text
    SetText(property.Get());

    // Subscribe to changes
    TrackSubscription(property, [this](const std::string& oldValue, const std::string& newValue) {
        SetText(newValue);
    });
}

void Button::PlayClickAnimation() {
    if (!m_element) {
        return;
    }

    Rml::PropertyDictionary pressed;
    Rml::PropertyDictionary released;
    const Rml::PropertyId transformId = Rml::PropertyId::Transform;
    if (Rml::StyleSheetSpecification::ParsePropertyDeclaration(
            pressed, "transform", "scale(0.96)") &&
        Rml::StyleSheetSpecification::ParsePropertyDeclaration(
            released, "transform", "scale(1.0)")) {
        const Rml::Property* pressedTransform = pressed.GetProperty(transformId);
        const Rml::Property* releasedTransform = released.GetProperty(transformId);
        if (pressedTransform && releasedTransform &&
            m_element->Animate("transform", *pressedTransform, 0.06f)) {
            m_element->AddAnimationKey("transform", *releasedTransform, 0.08f);
        }
    }
}

void Button::SetLoadingState(bool loading) {
    if (m_loading != loading) {
        m_loading = loading;

        if (m_element) {
            if (loading) {
                AddClass("loading");
                SetEnabled(false);
            } else {
                RemoveClass("loading");
                SetEnabled(true);
            }
        }
    }
}

void Button::UpdateStyles() {
    if (!m_element)
        return;

    // Apply component-specific styles
    m_element->SetProperty("cursor", "pointer");
}

void Button::UpdateClasses() {
    if (!m_element)
        return;

    // Remove all style classes
    RemoveClass("btn-primary");
    RemoveClass("btn-secondary");
    RemoveClass("btn-accent");
    RemoveClass("btn-success");
    RemoveClass("btn-danger");
    RemoveClass("btn-warning");

    // Remove all size classes
    RemoveClass("btn-sm");
    RemoveClass("btn-lg");

    // Add base button class
    AddClass("btn");

    // Add style class
    AddClass("btn-" + StyleToString(m_style));

    // Add size class
    if (m_size != Size::Normal) {
        AddClass("btn-" + SizeToString(m_size));
    }
}

void Button::HandleClick(Rml::Event& event) {
    if (!m_enabled || m_loading) {
        event.StopPropagation();
        return;
    }

    PlayClickAnimation();

    if (m_clickHandler) {
        m_clickHandler();
    }
}

void Button::HandleHover(bool isHovering) {
    if (!m_enabled)
        return;

    if (isHovering) {
        AddClass("hover");
    } else {
        RemoveClass("hover");
    }
}

std::string Button::StyleToString(Style style) {
    switch (style) {
        case Style::Primary:
            return "primary";
        case Style::Secondary:
            return "secondary";
        case Style::Accent:
            return "accent";
        case Style::Success:
            return "success";
        case Style::Danger:
            return "danger";
        case Style::Warning:
            return "warning";
        default:
            return "primary";
    }
}

std::string Button::SizeToString(Size size) {
    switch (size) {
        case Size::Small:
            return "sm";
        case Size::Normal:
            return "normal";
        case Size::Large:
            return "lg";
        default:
            return "normal";
    }
}

} // namespace Luminumbra::Client::UI
