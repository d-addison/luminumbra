#pragma once

#include "ui/core/UIComponent.h"
#include <functional>

namespace Luminumbra::Client::UI {

/**
 * Enhanced Button component with multiple styles and states
 */
class Button : public UIComponent {
public:
    enum class Style {
        Primary,
        Secondary,
        Accent,
        Success,
        Danger,
        Warning
    };
    
    enum class Size {
        Small,
        Normal,
        Large
    };

    explicit Button(const std::string& elementId);
    
    // Configuration
    void SetText(const std::string& text);
    void SetStyle(Style style);
    void SetSize(Size size);
    void SetEnabled(bool enabled);
    void SetIcon(const std::string& iconPath);
    
    // State
    bool IsEnabled() const { return m_enabled; }
    Style GetStyle() const { return m_style; }
    Size GetSize() const { return m_size; }
    
    // Events
    using ClickHandler = std::function<void()>;
    void SetClickHandler(ClickHandler handler);
    
    // Data binding
    void BindEnabled(Property<bool>& property);
    void BindText(Property<std::string>& property);
    
    // Animation
    void PlayClickAnimation();
    void SetLoadingState(bool loading);

protected:
    void OnElementSet() override;
    void UpdateStyles();
    void UpdateClasses();

private:
    Style m_style = Style::Primary;
    Size m_size = Size::Normal;
    bool m_enabled = true;
    bool m_loading = false;
    std::string m_text;
    std::string m_iconPath;
    ClickHandler m_clickHandler;
    
    void HandleClick(Rml::Event& event);
    void HandleHover(bool isHovering);
    
    static std::string StyleToString(Style style);
    static std::string SizeToString(Size size);
};

} // namespace Luminumbra::Client::UI