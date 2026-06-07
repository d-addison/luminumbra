#include "UIComponent.h"
#include "core/Log.h"

namespace Luminumbra::Client::UI {

UIComponent::UIComponent(const std::string& elementId) : m_elementId(elementId) {
}

void UIComponent::Initialize(Rml::ElementDocument* document) {
    m_document = document;
    if (document) {
        m_element = document->GetElementById(m_elementId);
        if (m_element) {
            OnElementSet();
        } else {
            LUMINUMBRA_CORE_WARN("[UI] Could not find element with ID: {}", m_elementId);
        }
    }
}

void UIComponent::Destroy() {
    CleanupBindings();
    m_eventListeners.clear();
    m_element = nullptr;
    m_document = nullptr;
}

void UIComponent::OnClick(ClickCallback callback) {
    if (!m_element || !callback) return;
    
    auto listener = std::make_unique<LambdaEventListener>(std::move(callback));
    m_element->AddEventListener("click", listener.get());
    m_eventListeners.emplace_back(std::move(listener));
}

void UIComponent::OnChange(ChangeCallback callback) {
    if (!m_element || !callback) return;
    
    auto listener = std::make_unique<LambdaEventListener>(std::move(callback));
    m_element->AddEventListener("change", listener.get());
    m_eventListeners.emplace_back(std::move(listener));
}

void UIComponent::OnHover(std::function<void(bool)> callback) {
    if (!m_element || !callback) return;
    
    auto enterListener = std::make_unique<LambdaEventListener>([callback](Rml::Event&) {
        callback(true);
    });
    
    auto leaveListener = std::make_unique<LambdaEventListener>([callback](Rml::Event&) {
        callback(false);
    });
    
    m_element->AddEventListener("mouseenter", enterListener.get());
    m_element->AddEventListener("mouseleave", leaveListener.get());
    
    m_eventListeners.emplace_back(std::move(enterListener));
    m_eventListeners.emplace_back(std::move(leaveListener));
}

void UIComponent::AddClass(const std::string& className) {
    if (m_element) {
        m_element->SetClass(className, true);
    }
}

void UIComponent::RemoveClass(const std::string& className) {
    if (m_element) {
        m_element->SetClass(className, false);
    }
}

void UIComponent::ToggleClass(const std::string& className) {
    if (m_element) {
        bool hasClass = m_element->IsClassSet(className);
        m_element->SetClass(className, !hasClass);
    }
}

bool UIComponent::HasClass(const std::string& className) const {
    return m_element ? m_element->IsClassSet(className) : false;
}

void UIComponent::SetAttribute(const std::string& name, const std::string& value) {
    if (m_element) {
        m_element->SetAttribute(name, value);
    }
}

std::string UIComponent::GetAttribute(const std::string& name) const {
    return m_element ? m_element->GetAttribute(name, "") : "";
}

void UIComponent::AnimateProperty(const std::string& property, const std::string& targetValue, float duration) {
    if (!m_element) return;
    
    // TODO: Implement smooth animations
    // For now, just set the property directly
    m_element->SetProperty(property, targetValue);
    
    LUMINUMBRA_CORE_TRACE("[UI] Animated property {} to {} on element {}", property, targetValue, m_elementId);
}

void UIComponent::FadeIn(float duration) {
    AnimateProperty("opacity", "1", duration);
    m_element->SetProperty("display", "block");
}

void UIComponent::FadeOut(float duration) {
    AnimateProperty("opacity", "0", duration);
    // TODO: Set display to none after animation completes
}

void UIComponent::CleanupBindings() {
    // Execute all cleanup functions
    for (auto& cleanup : m_bindings) {
        if (cleanup) {
            cleanup();
        }
    }
    m_bindings.clear();
}

} // namespace Luminumbra::Client::UI