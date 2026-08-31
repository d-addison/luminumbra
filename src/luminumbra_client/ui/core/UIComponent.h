#pragma once

#include "UIProperty.h"
#include "UIStateManager.h"
#include <RmlUi/Core.h>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace Luminumbra::Client::UI {

class LambdaEventListener : public Rml::EventListener {
public:
    using Callback = std::function<void(Rml::Event&)>;
    using DetachCallback = std::function<void()>;

    LambdaEventListener(Callback callback, DetachCallback detachCallback = {})
        : m_callback(std::move(callback))
        , m_detachCallback(std::move(detachCallback)) {}

    void ProcessEvent(Rml::Event& event) override {
        if (m_callback) {
            m_callback(event);
        }
    }

    void OnDetach(Rml::Element*) override {
        if (m_detachCallback) {
            m_detachCallback();
        }
    }

private:
    Callback m_callback;
    DetachCallback m_detachCallback;
};

/**
 * Base class for all UI components.
 * Provides data binding capabilities and common functionality.
 */
class UIComponent {
public:
    explicit UIComponent(const std::string& elementId);
    // Unsubscribes all tracked property subscriptions so a destroyed
    // component never receives further property notifications (UAF guard).
    virtual ~UIComponent();
    
    // Component lifecycle
    virtual void Initialize(Rml::ElementDocument* document);
    virtual void Update(float deltaTime) {}
    virtual void Destroy();
    
    // Element access
    Rml::Element* GetElement() const { return m_element; }
    const std::string& GetElementId() const { return m_elementId; }
    bool IsValid() const { return m_element != nullptr; }
    
    // Data binding methods
    template<typename T>
    void BindProperty(const std::string& attribute, Property<T>& property);
    
    template<typename T>
    void BindText(Property<T>& property);
    
    template<typename T>
    void BindValue(Property<T>& property);

    template<typename T, typename Predicate>
    void BindVisibility(Property<T>& property, Predicate&& visibilityFunc);

    // Event handling
    using ClickCallback = std::function<void(Rml::Event&)>;
    using ChangeCallback = std::function<void(Rml::Event&)>;
    
    void OnClick(ClickCallback callback);
    void OnChange(ChangeCallback callback);
    void OnHover(std::function<void(bool)> callback); // true = enter, false = leave
    
    // CSS class management
    void AddClass(const std::string& className);
    void RemoveClass(const std::string& className);
    void ToggleClass(const std::string& className);
    bool HasClass(const std::string& className) const;
    
    // Attribute management
    void SetAttribute(const std::string& name, const std::string& value);
    std::string GetAttribute(const std::string& name) const;
    
    // Animation support
    void AnimateProperty(const std::string& property, const std::string& targetValue, float duration);
    void FadeIn(float duration = 0.3f);
    void FadeOut(float duration = 0.3f);

protected:
    struct ListenerAttachment {
        Rml::Element* element = nullptr;
    };

    struct EventListenerBinding {
        std::shared_ptr<ListenerAttachment> attachment;
        Rml::String event;
        std::unique_ptr<LambdaEventListener> listener;
    };

    std::string m_elementId;
    Rml::Element* m_element = nullptr;
    Rml::ElementDocument* m_document = nullptr;

    // RAII property subscriptions; cleared (= unsubscribed) on Destroy
    // and in the destructor. The bound Property must outlive this component
    // (UI properties live in UIStateManager / manager singletons).
    std::vector<ScopedSubscription> m_bindings;

    // Event listeners for cleanup
    std::vector<EventListenerBinding> m_eventListeners;

    virtual void OnElementSet() {} // Called when element is first set
    void CleanupBindings();
    void CleanupEventListeners();
    void PruneDetachedEventListeners();
    void AddTrackedEventListener(Rml::Element* element,
                                 const Rml::String& event,
                                 LambdaEventListener::Callback callback);

    // Subscribe to a property and track the subscription for automatic
    // unsubscription when this component is destroyed. The callable type is
    // deduced independently so raw lambdas bind without conversion to
    // PropertyCallback<T> first.
    template<typename T, typename Callback>
    void TrackSubscription(Property<T>& property, Callback&& callback) {
        const SubscriptionToken token = property.Subscribe(std::forward<Callback>(callback));
        m_bindings.emplace_back(property, token);
    }
};

// Template implementations
template<typename T>
void UIComponent::BindProperty(const std::string& attribute, Property<T>& property) {
    if (!m_element) return;
    
    // Set initial value
    if constexpr (std::is_arithmetic_v<T>) {
        m_element->SetAttribute(attribute, std::to_string(property.Get()));
    } else {
        m_element->SetAttribute(attribute, property.Get());
    }
    
    // Subscribe to changes
    auto binding = [this, attribute](const T& oldValue, const T& newValue) {
        if (m_element) {
            if constexpr (std::is_arithmetic_v<T>) {
                m_element->SetAttribute(attribute, std::to_string(newValue));
            } else {
                m_element->SetAttribute(attribute, newValue);
            }
        }
    };
    
    TrackSubscription(property, std::move(binding));
}

template<typename T>
void UIComponent::BindText(Property<T>& property) {
    if (!m_element) return;
    
    // Set initial text
    if constexpr (std::is_arithmetic_v<T>) {
        m_element->SetInnerRML(std::to_string(property.Get()));
    } else {
        m_element->SetInnerRML(property.Get());
    }
    
    // Subscribe to changes
    auto binding = [this](const T& oldValue, const T& newValue) {
        if (m_element) {
            if constexpr (std::is_arithmetic_v<T>) {
                m_element->SetInnerRML(std::to_string(newValue));
            } else {
                m_element->SetInnerRML(newValue);
            }
        }
    };
    
    TrackSubscription(property, std::move(binding));
}

template<typename T>
void UIComponent::BindValue(Property<T>& property) {
    if (!m_element) return;
    
    // Set initial value
    if constexpr (std::is_arithmetic_v<T>) {
        m_element->SetAttribute("value", std::to_string(property.Get()));
    } else {
        m_element->SetAttribute("value", property.Get());
    }
    
    // Subscribe to property changes
    auto propertyBinding = [this](const T& oldValue, const T& newValue) {
        if (m_element) {
            if constexpr (std::is_arithmetic_v<T>) {
                m_element->SetAttribute("value", std::to_string(newValue));
            } else {
                m_element->SetAttribute("value", newValue);
            }
        }
    };
    
    TrackSubscription(property, std::move(propertyBinding));

    // Subscribe to element changes
    OnChange([&property](Rml::Event& event) {
        if constexpr (std::is_same_v<T, std::string>) {
            property.Set(event.GetTargetElement()->GetAttribute<Rml::String>("value", ""));
        } else if constexpr (std::is_same_v<T, int>) {
            property.Set(event.GetTargetElement()->GetAttribute("value", 0));
        } else if constexpr (std::is_same_v<T, float>) {
            property.Set(event.GetTargetElement()->GetAttribute("value", 0.0f));
        }
    });
}

template<typename T, typename Predicate>
void UIComponent::BindVisibility(Property<T>& property, Predicate&& visibilityFunc) {
    if (!m_element) return;

    std::function<bool(const T&)> predicate(std::forward<Predicate>(visibilityFunc));

    // Set initial visibility
    bool visible = predicate(property.Get());
    m_element->SetProperty("display", visible ? "block" : "none");
    
    // Subscribe to changes
    auto binding = [this, predicate = std::move(predicate)](const T& oldValue, const T& newValue) {
        if (m_element) {
            bool visible = predicate(newValue);
            m_element->SetProperty("display", visible ? "block" : "none");
        }
    };

    TrackSubscription(property, std::move(binding));
}

} // namespace Luminumbra::Client::UI
