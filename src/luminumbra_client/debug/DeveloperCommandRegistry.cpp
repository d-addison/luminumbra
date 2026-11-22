#include "debug/DeveloperCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <sstream>

namespace Luminumbra::Client::Debug {
namespace {
std::string LocalName(std::string name) {
    std::replace(name.begin(), name.end(), ' ', '.');
    return name;
}
Commands::Availability LocalAvailability(void* state) {
    const auto& def = *static_cast<const CommandDef*>(state);
    const auto availability = def.available ? def.available() : Availability{};
    return {availability.available, availability.reason};
}
Commands::Result ExecuteLocal(void* state, const Commands::Invocation& invocation) {
    const auto& def = *static_cast<const CommandDef*>(state);
    CommandArguments arguments;
    arguments.reserve(invocation.arguments.size());
    for (const auto& value : invocation.arguments) {
        std::visit([&](const auto& typed) { arguments.emplace_back(typed); }, value);
    }
    const auto result = def.execute(arguments);
    return {result.success ? Commands::Status::Ok : Commands::Status::Failed, result.message};
}
} // namespace
DeveloperCommandRegistry::DeveloperCommandRegistry(DeveloperWidgetRegistry& widgets,
                                                   DeveloperBindings& bindings)
    : m_widgets(widgets)
    , m_bindings(bindings) {}
bool DeveloperCommandRegistry::Register(CommandDef def) {
    if (m_sealed || def.name.empty() || !def.execute)
        return false;
    for (const auto& command : m_commands)
        if (command.name == def.name)
            return false;
    bool optional = false;
    for (const auto& arg : def.arguments) {
        if (optional && !arg.optional)
            return false;
        optional |= arg.optional;
    }
    m_commands.push_back(std::move(def));
    return true;
}
bool DeveloperCommandRegistry::RegisterLocal(CommandDef def) {
    if (m_sealed || def.name.size() > Commands::Registry::MaxName ||
        def.description.size() > Commands::Registry::MaxInput ||
        def.arguments.size() > Commands::Registry::MaxArguments ||
        m_localCommands.size() == Commands::Registry::MaxCommands) {
        return false;
    }
    Commands::Definition definition;
    definition.name = LocalName(def.name);
    definition.description = def.description;
    for (const auto& argument : def.arguments) {
        if (argument.name.size() > Commands::Registry::MaxName ||
            argument.values.size() > Commands::Registry::MaxChoices ||
            std::any_of(argument.values.begin(), argument.values.end(), [](const auto& value) {
                return value.size() > Commands::Registry::MaxInput;
            })) {
            return false;
        }
        Commands::ArgumentType type;
        switch (argument.type) {
            case ArgumentType::Bool:
                type = Commands::ArgumentType::Bool;
                break;
            case ArgumentType::Float:
                type = Commands::ArgumentType::Float;
                break;
            case ArgumentType::Enum:
                type = Commands::ArgumentType::Enum;
                break;
            default:
                return false;
        }
        definition.arguments.push_back({argument.name,
                                        type,
                                        argument.optional,
                                        argument.values,
                                        argument.minimum,
                                        argument.maximum});
    }
    if (!Register(def)) {
        return false;
    }
    m_localCommands.push_back(std::move(def));
    definition.state = &m_localCommands.back();
    definition.available = LocalAvailability;
    definition.execute = ExecuteLocal;
    if (!m_localRegistry.Register(definition)) {
        m_localCommands.pop_back();
        m_commands.pop_back();
        return false;
    }
    return true;
}
const Commands::Registry& DeveloperCommandRegistry::LocalCommands() const {
    return m_localRegistry;
}
void DeveloperCommandRegistry::Seal() {
    m_sealed = true;
    m_localRegistry.Seal();
}
const std::vector<CommandDef>& DeveloperCommandRegistry::List() const {
    return m_commands;
}
CommandResult DeveloperCommandRegistry::Dispatch(const std::string& name,
                                                 const std::vector<std::string>& arguments,
                                                 Commands::Context context) const {
    const bool local = std::any_of(m_localCommands.begin(),
                                   m_localCommands.end(),
                                   [&](const auto& def) { return def.name == name; });
    if (local) {
        const auto result = m_localRegistry.Dispatch(LocalName(name), arguments, context);
        if (result.status == Commands::Status::InvalidInput &&
            result.message.starts_with("Invalid arity:")) {
            return {false, "Invalid arity: " + Help(name)};
        }
        return {result.status == Commands::Status::Ok, result.message};
    }
    const auto it = std::find_if(m_commands.begin(), m_commands.end(), [&](const CommandDef& def) {
        return def.name == name;
    });
    if (it == m_commands.end())
        return {false, "Unknown command: " + name};
    const std::size_t required = static_cast<std::size_t>(
        std::count_if(it->arguments.begin(), it->arguments.end(), [](const CommandArgument& arg) {
            return !arg.optional;
        }));
    if (arguments.size() < required || arguments.size() > it->arguments.size())
        return {false, "Invalid arity: " + Help(name)};
    CommandArguments typed;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = it->arguments[i];
        const auto& value = arguments[i];
        switch (arg.type) {
            case ArgumentType::Bool:
                if (value != "true" && value != "false")
                    return {false, "Expected true or false"};
                typed.emplace_back(value == "true");
                break;
            case ArgumentType::Float: {
                float number = 0;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    !std::isfinite(number) || number < arg.minimum || number > arg.maximum)
                    return {false, "Invalid finite float: " + arg.name};
                typed.emplace_back(number);
                break;
            }
            case ArgumentType::Enum:
                if (!arg.values.empty() &&
                    std::find(arg.values.begin(), arg.values.end(), value) == arg.values.end())
                    return {false, "Unknown " + arg.name + ": " + value};
                typed.emplace_back(value);
                break;
            case ArgumentType::WidgetId:
                if (!m_widgets.Find(value))
                    return {false, "Unknown widget: " + value};
                typed.emplace_back(value);
                break;
            case ArgumentType::ActionId:
                if (!m_bindings.Contains(value))
                    return {false, "Unknown action: " + value};
                typed.emplace_back(value);
                break;
            case ArgumentType::KeyChord: {
                const auto chord = ParseKeyChord(value);
                if (!chord)
                    return {false, "Invalid key chord"};
                typed.emplace_back(*chord);
                break;
            }
        }
    }
    if (it->available) {
        const auto available = it->available();
        if (!available.available)
            return {false, available.reason};
    }
    return it->execute(typed);
}
CommandResult DeveloperCommandRegistry::Submit(const std::string& line) const {
    if (line.size() > kInputLimit)
        return {false, "Command exceeds 4 KiB"};
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false, started = false, escape = false;
    for (const char character : line) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '\n' || c == '\r' || c == 0)
            return {false, "Submit one command line"};
        if (escape) {
            if (c != '"' && c != '\\')
                return {false, "Invalid escape"};
            token += static_cast<char>(c);
            escape = false;
        } else if (c == '\\' && quoted)
            escape = true;
        else if (c == '"') {
            quoted = !quoted;
            started = true;
        } else if (std::isspace(c) && !quoted) {
            if (started) {
                tokens.push_back(token);
                token.clear();
                started = false;
            }
        } else {
            token += static_cast<char>(c);
            started = true;
        }
    }
    if (quoted || escape)
        return {false, "Unterminated quoted argument"};
    if (started)
        tokens.push_back(token);
    if (tokens.empty())
        return {};
    // Longest registered command prefix, allowing readable subcommands.
    const CommandDef* selected = nullptr;
    std::size_t prefix = 0;
    std::string name;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i)
            name += ' ';
        name += tokens[i];
        for (const auto& def : m_commands)
            if (def.name == name) {
                selected = &def;
                prefix = i + 1;
            }
    }
    if (!selected)
        return {false, "Unknown command: " + tokens.front()};
    return Dispatch(selected->name,
                    {tokens.begin() + static_cast<std::ptrdiff_t>(prefix), tokens.end()},
                    {Commands::EntryPoint::Console, Commands::Authority::Local});
}
std::string DeveloperCommandRegistry::Help(const std::string& name) const {
    std::ostringstream out;
    for (const auto& def : m_commands) {
        if (!name.empty() && def.name != name)
            continue;
        out << def.name;
        for (const auto& arg : def.arguments) {
            out << ' ' << (arg.optional ? '[' : '<') << arg.name;
            if (!arg.values.empty()) {
                out << ':';
                for (std::size_t i = 0; i < arg.values.size(); ++i) {
                    if (i)
                        out << '|';
                    out << arg.values[i];
                }
            }
            out << (arg.optional ? ']' : '>');
        }
        out << " — " << def.description;
        if (def.available) {
            const auto a = def.available();
            if (!a.available)
                out << " (unavailable: " << a.reason << ')';
        }
        for (const auto& [id, action] : m_actions)
            if (action.first == def.name)
                out << " [" << id << ": " << FormatKeyChord(m_bindings.Get(id)) << ']';
        out << '\n';
    }
    return out.str();
}
bool DeveloperCommandRegistry::BindAction(const std::string& id,
                                          const std::string& command,
                                          std::vector<std::string> arguments) {
    if (m_sealed || !m_bindings.Contains(id) || m_actions.contains(id))
        return false;
    m_actions.emplace(id, std::make_pair(command, std::move(arguments)));
    return true;
}
CommandResult DeveloperCommandRegistry::InvokeAction(const std::string& id) const {
    const auto it = m_actions.find(id);
    if (it == m_actions.end())
        return {false, "Unknown action: " + id};
    return Dispatch(it->second.first,
                    it->second.second,
                    {Commands::EntryPoint::Shortcut, Commands::Authority::Local});
}
} // namespace Luminumbra::Client::Debug
