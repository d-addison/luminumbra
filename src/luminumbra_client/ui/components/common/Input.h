#pragma once

#include "ui/core/UIComponent.h"
#include <functional>
#include <regex>

namespace Luminumbra::Client::UI {

/**
 * Enhanced Input component with validation and different types
 */
class Input : public UIComponent {
public:
    enum class Type {
        Text,
        Number,
        Email,
        Password
    };
    
    enum class ValidationState {
        None,
        Valid,
        Invalid
    };

    explicit Input(const std::string& elementId);
    
    // Configuration
    void SetType(Type type);
    void SetPlaceholder(const std::string& placeholder);
    void SetValue(const std::string& value);
    void SetEnabled(bool enabled);
    void SetRequired(bool required);
    void SetMaxLength(int maxLength);
    
    // Validation
    void SetValidationPattern(const std::string& pattern);
    void SetValidationMessage(const std::string& message);
    void SetCustomValidator(std::function<bool(const std::string&)> validator);
    bool Validate();
    void ClearValidation();
    
    // State
    std::string GetValue() const;
    bool IsEnabled() const { return m_enabled; }
    bool IsRequired() const { return m_required; }
    bool IsValid() const;
    ValidationState GetValidationState() const { return m_validationState; }
    
    // Events
    using ChangeHandler = std::function<void(const std::string&)>;
    using ValidationHandler = std::function<void(bool)>;
    
    void SetChangeHandler(ChangeHandler handler);
    void SetValidationHandler(ValidationHandler handler);
    
    // Data binding
    void BindValue(Property<std::string>& property);
    void BindEnabled(Property<bool>& property);
    
    // Focus management
    void Focus();
    void Blur();

protected:
    void OnElementSet() override;
    void UpdateValidationState();
    void ShowValidationMessage(const std::string& message);
    void HideValidationMessage();

private:
    Type m_type = Type::Text;
    bool m_enabled = true;
    bool m_required = false;
    int m_maxLength = -1;
    std::string m_placeholder;
    std::string m_value;
    std::string m_validationPattern;
    std::string m_validationMessage;
    ValidationState m_validationState = ValidationState::None;
    
    std::function<bool(const std::string&)> m_customValidator;
    ChangeHandler m_changeHandler;
    ValidationHandler m_validationHandler;
    
    std::regex m_compiledPattern;
    bool m_patternCompiled = false;
    
    void HandleChange(Rml::Event& event);
    void HandleFocus(Rml::Event& event);
    void HandleBlur(Rml::Event& event);
    void HandleInput(Rml::Event& event);
    
    bool ValidateInternal(const std::string& value);
    void CompileValidationPattern();
    
    static std::string TypeToString(Type type);
};

} // namespace Luminumbra::Client::UI