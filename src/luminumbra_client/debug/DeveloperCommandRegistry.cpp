#include "debug/DeveloperCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <sstream>

namespace Luminumbra::Client::Debug {
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
void DeveloperCommandRegistry::Seal() {
    m_sealed = true;
}
const std::vector<CommandDef>& DeveloperCommandRegistry::List() const {
    return m_commands;
}
CommandResult DeveloperCommandRegistry::Dispatch(const std::string& name,
                                                 const std::vector<std::string>& arguments) const {
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
    for (const unsigned char c : line) {
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
                    {tokens.begin() + static_cast<std::ptrdiff_t>(prefix), tokens.end()});
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
    return Dispatch(it->second.first, it->second.second);
}
} // namespace Luminumbra::Client::Debug
