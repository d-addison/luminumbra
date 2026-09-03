#pragma once

#include "ui/core/UIComponent.h"

namespace Luminumbra::Client::UI {

/**
 * Panel component for organizing UI content
 */
class Panel : public UIComponent {
public:
    enum class Style {
        Default,
        Elevated,
        Bordered,
        Transparent
    };

    explicit Panel(const std::string& elementId);

    // Configuration
    void SetTitle(const std::string& title);
    void SetStyle(Style style);
    void SetCollapsible(bool collapsible);
    void SetExpanded(bool expanded);

    // Content management
    void SetContent(const std::string& content);
    void AppendContent(const std::string& content);
    void ClearContent();

    // State
    bool IsExpanded() const {
        return m_expanded;
    }
    bool IsCollapsible() const {
        return m_collapsible;
    }
    Style GetStyle() const {
        return m_style;
    }

    // Events
    using ToggleHandler = std::function<void(bool)>; // expanded state
    void SetToggleHandler(ToggleHandler handler);

    // Data binding
    void BindTitle(Property<std::string>& property);
    void BindExpanded(Property<bool>& property);
    void BindVisible(Property<bool>& property);

protected:
    void OnElementSet() override;
    void UpdateStyles();
    void CreateHeader();
    void CreateBody();

private:
    Style m_style = Style::Default;
    bool m_collapsible = false;
    bool m_expanded = true;
    std::string m_title;
    ToggleHandler m_toggleHandler;

    Rml::Element* m_headerElement = nullptr;
    Rml::Element* m_bodyElement = nullptr;
    Rml::Element* m_toggleButton = nullptr;

    void HandleToggle();
    void UpdateExpandedState();

    static std::string StyleToString(Style style);
};

} // namespace Luminumbra::Client::UI