#include "Panel.h"
#include "core/Log.h"
#include <RmlUi/Core/StringUtilities.h>

namespace Luminumbra::Client::UI {

Panel::Panel(const std::string& elementId) : UIComponent(elementId) {
}

void Panel::OnElementSet() {
    if (!m_element) return;

    UpdateStyles();
    CreateHeader();
    CreateBody();

    const bool collapsible = m_collapsible;
    m_collapsible = false;
    SetCollapsible(collapsible);
    UpdateExpandedState();

    if (!m_title.empty()) {
        SetTitle(m_title);
    }
}

void Panel::SetTitle(const std::string& title) {
    m_title = title;
    
    if (!m_headerElement && !title.empty()) {
        CreateHeader();
    }
    
    if (m_headerElement) {
        if (title.empty()) {
            m_headerElement->SetProperty("display", "none");
        } else {
            m_headerElement->SetProperty("display", "block");
            
            // Find or create title element
            Rml::Element* titleElement = m_headerElement->GetElementById(m_elementId + "_title");
            if (!titleElement) {
                // Create title element
                auto title = m_document->CreateElement("div");
                titleElement = title.get();
                titleElement->SetId(m_elementId + "_title");
                titleElement->SetClass("panel-title", true);
                m_headerElement->AppendChild(std::move(title));
            }

            titleElement->SetInnerRML(Rml::StringUtilities::EncodeRml(title));
        }
    }
}

void Panel::SetStyle(Style style) {
    if (m_style != style) {
        m_style = style;
        UpdateStyles();
    }
}

void Panel::SetCollapsible(bool collapsible) {
    if (m_collapsible != collapsible) {
        m_collapsible = collapsible;
        
        if (collapsible && !m_toggleButton) {
            // Create toggle button
            if (!m_headerElement) {
                CreateHeader();
            }
            
            if (m_headerElement) {
                auto toggle = m_document->CreateElement("button");
                m_toggleButton = toggle.get();
                m_toggleButton->SetId(m_elementId + "_toggle");
                m_toggleButton->SetClass("panel-toggle", true);
                m_toggleButton->SetInnerRML(m_expanded ? "−" : "+");

                AddTrackedEventListener(
                    m_toggleButton, "click", [this](Rml::Event&) { HandleToggle(); });
                m_headerElement->AppendChild(std::move(toggle));
            }
        } else if (!collapsible && m_toggleButton) {
            // Remove toggle button
            if (m_headerElement) {
                m_headerElement->RemoveChild(m_toggleButton);
            }
            m_toggleButton = nullptr;
            PruneDetachedEventListeners();
        }
    }
}

void Panel::SetExpanded(bool expanded) {
    if (m_expanded != expanded) {
        m_expanded = expanded;
        UpdateExpandedState();
        
        if (m_toggleHandler) {
            m_toggleHandler(expanded);
        }
    }
}

void Panel::SetContent(const std::string& content) {
    if (!m_bodyElement) {
        CreateBody();
    }

    if (m_bodyElement) {
        m_bodyElement->SetInnerRML(content);
    }
}

void Panel::AppendContent(const std::string& content) {
    if (!m_bodyElement) {
        CreateBody();
    }

    if (m_bodyElement) {
        std::string currentContent = m_bodyElement->GetInnerRML();
        m_bodyElement->SetInnerRML(currentContent + content);
    }
}

void Panel::ClearContent() {
    if (m_bodyElement) {
        m_bodyElement->SetInnerRML("");
    }
}

void Panel::SetToggleHandler(ToggleHandler handler) {
    m_toggleHandler = std::move(handler);
}

void Panel::BindTitle(Property<std::string>& property) {
    // Set initial title
    SetTitle(property.Get());

    // Subscribe to changes
    TrackSubscription(property, [this](const std::string& oldValue, const std::string& newValue) {
        SetTitle(newValue);
    });
}

void Panel::BindExpanded(Property<bool>& property) {
    // Set initial state
    SetExpanded(property.Get());
    
    // Subscribe to property changes
    TrackSubscription(property, [this](const bool& oldValue, const bool& newValue) {
        SetExpanded(newValue);
    });
    
    // Update property when panel is toggled
    SetToggleHandler([&property](bool expanded) {
        property.Set(expanded);
    });
}

void Panel::BindVisible(Property<bool>& property) {
    BindVisibility(property, [](const bool& visible) { return visible; });
}

void Panel::UpdateStyles() {
    if (!m_element) return;
    
    // Remove existing style classes
    RemoveClass("panel");
    RemoveClass("panel-elevated");
    RemoveClass("panel-bordered");
    RemoveClass("panel-transparent");
    
    // Add base panel class
    AddClass("panel");
    
    // Add style-specific class
    if (m_style != Style::Default) {
        AddClass("panel-" + StyleToString(m_style));
    }
}

void Panel::CreateHeader() {
    if (m_headerElement || !m_element) return;

    auto header = m_document->CreateElement("div");
    m_headerElement = header.get();
    m_headerElement->SetId(m_elementId + "_header");
    m_headerElement->SetClass("panel-header", true);
    
    // Insert header as first child
    if (m_element->GetFirstChild()) {
        m_element->InsertBefore(std::move(header), m_element->GetFirstChild());
    } else {
        m_element->AppendChild(std::move(header));
    }
}

void Panel::CreateBody() {
    if (m_bodyElement || !m_element) return;

    auto body = m_document->CreateElement("div");
    m_bodyElement = body.get();
    m_bodyElement->SetId(m_elementId + "_body");
    m_bodyElement->SetClass("panel-body", true);

    m_element->AppendChild(std::move(body));
}

void Panel::HandleToggle() {
    SetExpanded(!m_expanded);
}

void Panel::UpdateExpandedState() {
    if (m_bodyElement) {
        m_bodyElement->SetProperty("display", m_expanded ? "block" : "none");
    }
    
    if (m_toggleButton) {
        m_toggleButton->SetInnerRML(m_expanded ? "−" : "+");
    }
    
    if (m_element) {
        if (m_expanded) {
            RemoveClass("collapsed");
            AddClass("expanded");
        } else {
            RemoveClass("expanded");
            AddClass("collapsed");
        }
    }
}

std::string Panel::StyleToString(Style style) {
    switch (style) {
        case Style::Default: return "default";
        case Style::Elevated: return "elevated";
        case Style::Bordered: return "bordered";
        case Style::Transparent: return "transparent";
        default: return "default";
    }
}

} // namespace Luminumbra::Client::UI
