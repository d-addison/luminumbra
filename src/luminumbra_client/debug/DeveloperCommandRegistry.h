#pragma once

#include "debug/DeveloperWidgetRegistry.h"

#include <variant>

namespace Luminumbra::Client::Debug {
enum class ArgumentType {
    Bool,
    Float,
    Enum,
    WidgetId,
    ActionId,
    KeyChord
};
using CommandValue = std::variant<bool, float, std::string, KeyChord>;
struct CommandArgument {
    std::string name;
    ArgumentType type;
    bool optional = false;
    std::vector<std::string> values = {};
    float minimum = 0;
    float maximum = 16;
};
struct CommandResult {
    bool success = true;
    std::string message = {};
};
using CommandArguments = std::vector<CommandValue>;
struct CommandDef {
    std::string name;
    std::string description;
    std::vector<CommandArgument> arguments = {};
    AvailabilityFn available = {};
    std::function<CommandResult(const CommandArguments&)> execute;
};
class DeveloperCommandRegistry {
public:
    static constexpr std::size_t kInputLimit = 4096;
    DeveloperCommandRegistry(DeveloperWidgetRegistry& widgets, DeveloperBindings& bindings);
    bool Register(CommandDef def);
    void Seal();
    const std::vector<CommandDef>& List() const;
    CommandResult Dispatch(const std::string& name,
                           const std::vector<std::string>& arguments = {}) const;
    CommandResult Submit(const std::string& line) const;
    std::string Help(const std::string& name = {}) const;
    // Shortcuts and buttons dispatch through the same validated path as text submissions.
    bool BindAction(const std::string& id,
                    const std::string& command,
                    std::vector<std::string> arguments = {});
    CommandResult InvokeAction(const std::string& id) const;

private:
    DeveloperWidgetRegistry& m_widgets;
    DeveloperBindings& m_bindings;
    std::vector<CommandDef> m_commands;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> m_actions;
    bool m_sealed = false;
};
} // namespace Luminumbra::Client::Debug
