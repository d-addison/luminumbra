#include "UIComponent.h"
#include "core/Log.h"
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <algorithm>

namespace Luminumbra::Client::UI {

UIComponent::UIComponent(const std::string& elementId)
    : m_elementId(elementId) {}

UIComponent::~UIComponent() {
    // Unsubscribe all property subscriptions even if Destroy was never
    // called, so a destroyed component cannot be invoked by a later
    // Property::Set (use-after-free).
    CleanupBindings();
    CleanupEventListeners();
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
    CleanupEventListeners();
    m_element = nullptr;
    m_document = nullptr;
}

void UIComponent::OnClick(ClickCallback callback) {
    if (!m_element || !callback)
        return;

    AddTrackedEventListener(m_element, "click", std::move(callback));
}

void UIComponent::OnChange(ChangeCallback callback) {
    if (!m_element || !callback)
        return;

    AddTrackedEventListener(m_element, "change", std::move(callback));
}

void UIComponent::OnHover(std::function<void(bool)> callback) {
    if (!m_element || !callback)
        return;

    AddTrackedEventListener(m_element, "mouseenter", [callback](Rml::Event&) { callback(true); });
    AddTrackedEventListener(m_element, "mouseleave", [callback](Rml::Event&) { callback(false); });
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
    return m_element ? m_element->GetAttribute<Rml::String>(name, "") : std::string{};
}

void UIComponent::AnimateProperty(const std::string& property,
                                  const std::string& targetValue,
                                  float duration) {
    if (!m_element)
        return;

    Rml::PropertyDictionary properties;
    const Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(property);
    if (id == Rml::PropertyId::Invalid || !Rml::StyleSheetSpecification::ParsePropertyDeclaration(
                                              properties, property, targetValue)) {
        LUMINUMBRA_CORE_WARN("[UI] Could not animate invalid property {}={} on element {}",
                             property,
                             targetValue,
                             m_elementId);
        return;
    }
    const Rml::Property* target = properties.GetProperty(id);
    if (!target || !m_element->Animate(property, *target, std::max(0.0f, duration))) {
        LUMINUMBRA_CORE_WARN("[UI] Could not start animation {}={} on element {}",
                             property,
                             targetValue,
                             m_elementId);
    }
}

void UIComponent::FadeIn(float duration) {
    if (!m_element)
        return;
    m_element->SetProperty("display", "block");
    m_element->SetProperty("pointer-events", "auto");
    AnimateProperty("opacity", "1", duration);
}

void UIComponent::FadeOut(float duration) {
    if (!m_element)
        return;
    AnimateProperty("opacity", "0", duration);
    m_element->SetProperty("pointer-events", "none");
}

void UIComponent::CleanupBindings() {
    // ScopedSubscription destructors unsubscribe from the bound properties.
    m_bindings.clear();
}

void UIComponent::CleanupEventListeners() {
    for (auto& binding : m_eventListeners) {
        if (binding.attachment && binding.attachment->element && binding.listener) {
            binding.attachment->element->RemoveEventListener(binding.event, binding.listener.get());
        }
    }
    m_eventListeners.clear();
}

void UIComponent::PruneDetachedEventListeners() {
    std::erase_if(m_eventListeners, [](const EventListenerBinding& binding) {
        return !binding.attachment || !binding.attachment->element;
    });
}

void UIComponent::AddTrackedEventListener(Rml::Element* element,
                                          const Rml::String& event,
                                          LambdaEventListener::Callback callback) {
    if (!element || !callback) {
        return;
    }
    auto attachment = std::make_shared<ListenerAttachment>();
    attachment->element = element;
    auto listener = std::make_unique<LambdaEventListener>(
        std::move(callback), [attachment]() { attachment->element = nullptr; });
    element->AddEventListener(event, listener.get());
    m_eventListeners.push_back({std::move(attachment), event, std::move(listener)});
}

} // namespace Luminumbra::Client::UI
