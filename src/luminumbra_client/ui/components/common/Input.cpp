#include "Input.h"
#include "core/Log.h"
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/StringUtilities.h>

namespace Luminumbra::Client::UI {

Input::Input(const std::string& elementId)
    : UIComponent(elementId) {}

void Input::OnElementSet() {
    if (!m_element)
        return;

    // Set up event handlers
    OnChange([this](Rml::Event& event) { HandleChange(event); });

    // Set up focus/blur handlers
    AddTrackedEventListener(m_element, "focus", [this](Rml::Event& event) { HandleFocus(event); });
    AddTrackedEventListener(m_element, "blur", [this](Rml::Event& event) { HandleBlur(event); });
    AddTrackedEventListener(m_element, "input", [this](Rml::Event& event) { HandleInput(event); });

    // Apply initial configuration
    SetType(m_type);
    if (!m_placeholder.empty()) {
        SetPlaceholder(m_placeholder);
    }
    if (!m_value.empty()) {
        SetValue(m_value);
    }
    SetEnabled(m_enabled);
    SetRequired(m_required);
    if (m_maxLength > 0) {
        SetMaxLength(m_maxLength);
    }
}

void Input::SetType(Type type) {
    m_type = type;
    if (m_element) {
        m_element->SetAttribute("type", TypeToString(type));
    }
}

void Input::SetPlaceholder(const std::string& placeholder) {
    m_placeholder = placeholder;
    if (m_element) {
        m_element->SetAttribute("placeholder", placeholder);
    }
}

void Input::SetValue(const std::string& value) {
    m_value = value;
    if (m_element) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(m_element)) {
            control->SetValue(value);
        } else {
            m_element->SetAttribute("value", value);
        }
    }

    // Validate new value
    if (!value.empty()) {
        Validate();
    }
}

void Input::SetEnabled(bool enabled) {
    m_enabled = enabled;
    if (m_element) {
        if (enabled) {
            m_element->RemoveAttribute("disabled");
            RemoveClass("disabled");
        } else {
            m_element->SetAttribute("disabled", "");
            AddClass("disabled");
        }
    }
}

void Input::SetRequired(bool required) {
    m_required = required;
    if (m_element) {
        if (required) {
            m_element->SetAttribute("required", "");
            AddClass("required");
        } else {
            m_element->RemoveAttribute("required");
            RemoveClass("required");
        }
    }
}

void Input::SetMaxLength(int maxLength) {
    m_maxLength = maxLength;
    if (m_element) {
        if (maxLength > 0) {
            m_element->SetAttribute("maxlength", std::to_string(maxLength));
        } else {
            m_element->RemoveAttribute("maxlength");
        }
    }
}

void Input::SetValidationPattern(const std::string& pattern) {
    m_validationPattern = pattern;
    m_patternCompiled = false;
    CompileValidationPattern();
}

void Input::SetValidationMessage(const std::string& message) {
    m_validationMessage = message;
}

void Input::SetCustomValidator(std::function<bool(const std::string&)> validator) {
    m_customValidator = std::move(validator);
}

bool Input::Validate() {
    std::string currentValue = GetValue();
    bool isValid = ValidateInternal(currentValue);

    ValidationState newState = isValid ? ValidationState::Valid : ValidationState::Invalid;
    if (m_validationState != newState) {
        m_validationState = newState;
        UpdateValidationState();

        if (m_validationHandler) {
            m_validationHandler(isValid);
        }
    }

    return isValid;
}

void Input::ClearValidation() {
    if (m_validationState != ValidationState::None) {
        m_validationState = ValidationState::None;
        UpdateValidationState();
    }
}

std::string Input::GetValue() const {
    if (auto* control = dynamic_cast<Rml::ElementFormControl*>(m_element)) {
        return control->GetValue();
    }
    return m_element ? m_element->GetAttribute<Rml::String>("value", "") : m_value;
}

bool Input::IsValid() const {
    return m_validationState == ValidationState::Valid ||
           m_validationState == ValidationState::None;
}

void Input::SetChangeHandler(ChangeHandler handler) {
    m_changeHandler = std::move(handler);
}

void Input::SetValidationHandler(ValidationHandler handler) {
    m_validationHandler = std::move(handler);
}

void Input::BindValue(Property<std::string>& property) {
    // Set initial value
    SetValue(property.Get());

    // Subscribe to property changes
    TrackSubscription(property, [this](const std::string& oldValue, const std::string& newValue) {
        SetValue(newValue);
    });

    // Update property when input changes
    SetChangeHandler([&property](const std::string& newValue) { property.Set(newValue); });
}

void Input::BindEnabled(Property<bool>& property) {
    // Set initial state
    SetEnabled(property.Get());

    // Subscribe to changes
    TrackSubscription(property,
                      [this](const bool& oldValue, const bool& newValue) { SetEnabled(newValue); });
}

void Input::Focus() {
    if (m_element) {
        m_element->Focus();
    }
}

void Input::Blur() {
    if (m_element) {
        m_element->Blur();
    }
}

void Input::UpdateValidationState() {
    if (!m_element)
        return;

    // Remove existing validation classes
    RemoveClass("input-error");
    RemoveClass("input-success");

    switch (m_validationState) {
        case ValidationState::Valid:
            AddClass("input-success");
            HideValidationMessage();
            break;

        case ValidationState::Invalid:
            AddClass("input-error");
            ShowValidationMessage(m_validationMessage);
            break;

        case ValidationState::None:
            HideValidationMessage();
            break;
    }
}

void Input::ShowValidationMessage(const std::string& message) {
    if (!m_element || message.empty())
        return;

    // Look for existing error message element
    Rml::Element* errorElement = nullptr;
    if (auto* parent = m_element->GetParentNode()) {
        errorElement = parent->GetElementById(m_elementId + "_error");
    }

    if (!errorElement) {
        // Create error message element
        if (auto* parent = m_element->GetParentNode()) {
            auto error = m_document->CreateElement("div");
            errorElement = error.get();
            errorElement->SetId(m_elementId + "_error");
            errorElement->SetClass("field-error", true);
            errorElement->SetInnerRML(Rml::StringUtilities::EncodeRml(message));
            if (Rml::Element* next = m_element->GetNextSibling()) {
                parent->InsertBefore(std::move(error), next);
            } else {
                parent->AppendChild(std::move(error));
            }
        }
    } else {
        errorElement->SetInnerRML(Rml::StringUtilities::EncodeRml(message));
        errorElement->SetProperty("display", "block");
    }
}

void Input::HideValidationMessage() {
    if (!m_element)
        return;

    if (auto* parent = m_element->GetParentNode()) {
        if (auto* errorElement = parent->GetElementById(m_elementId + "_error")) {
            errorElement->SetProperty("display", "none");
        }
    }
}

void Input::HandleChange(Rml::Event& event) {
    auto* control = dynamic_cast<Rml::ElementFormControl*>(event.GetTargetElement());
    std::string newValue = control
                               ? control->GetValue()
                               : event.GetTargetElement()->GetAttribute<Rml::String>("value", "");
    m_value = newValue;

    // Validate on change
    Validate();

    if (m_changeHandler) {
        m_changeHandler(newValue);
    }
}

void Input::HandleFocus(Rml::Event& event) {
    AddClass("focused");
}

void Input::HandleBlur(Rml::Event& event) {
    RemoveClass("focused");

    // Validate on blur
    Validate();
}

void Input::HandleInput(Rml::Event& event) {
    // Real-time input validation (less strict)
    auto* control = dynamic_cast<Rml::ElementFormControl*>(event.GetTargetElement());
    std::string newValue = control
                               ? control->GetValue()
                               : event.GetTargetElement()->GetAttribute<Rml::String>("value", "");
    m_value = newValue;
}

bool Input::ValidateInternal(const std::string& value) {
    // Check required field
    if (m_required && value.empty()) {
        return false;
    }

    // Skip validation for empty optional fields
    if (!m_required && value.empty()) {
        return true;
    }

    // Check custom validator first
    if (m_customValidator && !m_customValidator(value)) {
        return false;
    }

    // Check pattern validation
    if (m_patternCompiled) {
        if (!std::regex_match(value, m_compiledPattern)) {
            return false;
        }
    }

    // Type-specific validation
    switch (m_type) {
        case Type::Email: {
            std::regex emailPattern(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
            return std::regex_match(value, emailPattern);
        }

        case Type::Number: {
            try {
                std::stod(value);
                return true;
            } catch (...) {
                return false;
            }
        }

        case Type::Text:
        case Type::Password:
        default:
            return true;
    }
}

void Input::CompileValidationPattern() {
    if (!m_validationPattern.empty() && !m_patternCompiled) {
        try {
            m_compiledPattern = std::regex(m_validationPattern);
            m_patternCompiled = true;
        } catch (const std::exception& e) {
            LUMINUMBRA_CORE_ERROR("[UI] Failed to compile validation pattern '{}': {}",
                                  m_validationPattern,
                                  e.what());
            m_patternCompiled = false;
        }
    }
}

std::string Input::TypeToString(Type type) {
    switch (type) {
        case Type::Text:
            return "text";
        case Type::Number:
            return "number";
        case Type::Email:
            return "email";
        case Type::Password:
            return "password";
        default:
            return "text";
    }
}

} // namespace Luminumbra::Client::UI
