#include "Input.h"
#include "core/Log.h"

namespace Luminumbra::Client::UI {

Input::Input(const std::string& elementId) : UIComponent(elementId) {
}

void Input::OnElementSet() {
    if (!m_element) return;
    
    // Set up event handlers
    OnChange([this](Rml::Event& event) { HandleChange(event); });
    
    // Set up focus/blur handlers
    auto focusListener = std::make_unique<LambdaEventListener>([this](Rml::Event& event) { HandleFocus(event); });
    auto blurListener = std::make_unique<LambdaEventListener>([this](Rml::Event& event) { HandleBlur(event); });
    auto inputListener = std::make_unique<LambdaEventListener>([this](Rml::Event& event) { HandleInput(event); });
    
    m_element->AddEventListener("focus", focusListener.get());
    m_element->AddEventListener("blur", blurListener.get());
    m_element->AddEventListener("input", inputListener.get());
    
    m_eventListeners.emplace_back(std::move(focusListener));
    m_eventListeners.emplace_back(std::move(blurListener));
    m_eventListeners.emplace_back(std::move(inputListener));
    
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
        m_element->SetAttribute("value", value);
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
    if (m_element && maxLength > 0) {
        m_element->SetAttribute("maxlength", std::to_string(maxLength));
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
    return m_element ? m_element->GetAttribute("value", "") : m_value;
}

bool Input::IsValid() const {
    return m_validationState == ValidationState::Valid || m_validationState == ValidationState::None;
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
    property.Subscribe([this](const std::string& oldValue, const std::string& newValue) {
        SetValue(newValue);
    });
    
    // Update property when input changes
    SetChangeHandler([&property](const std::string& newValue) {
        property.Set(newValue);
    });
}

void Input::BindEnabled(Property<bool>& property) {
    // Set initial state
    SetEnabled(property.Get());
    
    // Subscribe to changes
    property.Subscribe([this](const bool& oldValue, const bool& newValue) {
        SetEnabled(newValue);
    });
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
    if (!m_element) return;
    
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
    if (!m_element || message.empty()) return;
    
    // Look for existing error message element
    Rml::Element* errorElement = nullptr;
    if (auto* parent = m_element->GetParentNode()) {
        errorElement = parent->GetElementById(m_elementId + "_error");
    }
    
    if (!errorElement) {
        // Create error message element
        if (auto* parent = m_element->GetParentNode()) {
            std::string errorHtml = "<div id=\"" + m_elementId + "_error\" class=\"field-error\">" + message + "</div>";
            // TODO: Insert after input element
        }
    } else {
        errorElement->SetInnerRML(message);
        errorElement->SetProperty("display", "block");
    }
}

void Input::HideValidationMessage() {
    if (!m_element) return;
    
    if (auto* parent = m_element->GetParentNode()) {
        if (auto* errorElement = parent->GetElementById(m_elementId + "_error")) {
            errorElement->SetProperty("display", "none");
        }
    }
}

void Input::HandleChange(Rml::Event& event) {
    std::string newValue = event.GetTargetElement()->GetAttribute("value", "");
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
    std::string newValue = event.GetTargetElement()->GetAttribute("value", "");
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
            LUMINUMBRA_CORE_ERROR("[UI] Failed to compile validation pattern '{}': {}", m_validationPattern, e.what());
            m_patternCompiled = false;
        }
    }
}

std::string Input::TypeToString(Type type) {
    switch (type) {
        case Type::Text: return "text";
        case Type::Number: return "number";
        case Type::Email: return "email";
        case Type::Password: return "password";
        default: return "text";
    }
}

} // namespace Luminumbra::Client::UI