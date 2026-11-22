#include "core/CommandRegistry.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace Luminumbra::Commands {
namespace {
bool ValidName(std::string_view name, bool namespaced) {
    if (name.empty() || name.size() > Registry::MaxName || name.front() == '.' ||
        name.back() == '.' || name.find("..") != std::string_view::npos ||
        (namespaced && name.find('.') == std::string_view::npos)) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}
bool ValidText(std::string_view text) {
    return text.size() <= Registry::MaxInput &&
           text.find_first_of("\r\n\0", 0, 3) == std::string_view::npos;
}
bool ValidArgument(const Argument& argument) {
    if (!ValidName(argument.name, false) || argument.choices.size() > Registry::MaxChoices) {
        return false;
    }
    switch (argument.type) {
        case ArgumentType::Float:
            return argument.choices.empty() && std::isfinite(argument.minimum) &&
                   std::isfinite(argument.maximum) && argument.minimum <= argument.maximum;
        case ArgumentType::Enum:
            return !argument.choices.empty() &&
                   std::all_of(argument.choices.begin(), argument.choices.end(), [](const auto& v) {
                       return !v.empty() && ValidText(v);
                   });
        case ArgumentType::Bool:
        case ArgumentType::String:
            return argument.choices.empty();
    }
    return false;
}
Result Parse(const Argument& argument, const std::string& text, std::vector<Value>& values) {
    switch (argument.type) {
        case ArgumentType::Bool:
            if (text != "true" && text != "false") {
                return {Status::InvalidInput, "Expected true or false"};
            }
            values.emplace_back(text == "true");
            break;
        case ArgumentType::Float: {
            float number = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                !std::isfinite(number) || number < argument.minimum || number > argument.maximum) {
                return {Status::InvalidInput, "Invalid finite float: " + argument.name};
            }
            values.emplace_back(number);
            break;
        }
        case ArgumentType::Enum:
            if (std::find(argument.choices.begin(), argument.choices.end(), text) ==
                argument.choices.end()) {
                return {Status::InvalidInput, "Unknown " + argument.name + ": " + text};
            }
            values.emplace_back(text);
            break;
        case ArgumentType::String:
            values.emplace_back(text);
            break;
    }
    return {};
}
} // namespace

bool Registry::Register(const Definition& definition) {
    if (m_sealed || m_definitions.size() == MaxCommands || !definition.execute ||
        !ValidName(definition.name, true) || definition.description.size() > MaxInput ||
        definition.arguments.size() > MaxArguments || Find(definition.name) ||
        (definition.authority != Authority::Local &&
         definition.authority != Authority::Authoritative)) {
        return false;
    }
    std::size_t bytes = definition.name.size() + definition.description.size();
    if (bytes > MaxInput) {
        return false;
    }
    bool optional = false;
    for (const auto& argument : definition.arguments) {
        if (!ValidArgument(argument) || (optional && !argument.optional)) {
            return false;
        }
        if (argument.name.size() > MaxInput - bytes) {
            return false;
        }
        bytes += argument.name.size();
        for (const auto& choice : argument.choices) {
            if (choice.size() > MaxInput - bytes) {
                return false;
            }
            bytes += choice.size();
        }
        optional |= argument.optional;
    }
    m_definitions.push_back(definition);
    return true;
}
void Registry::Seal() {
    m_sealed = true;
}
const Definition* Registry::Find(std::string_view name) const {
    const auto found = std::find_if(m_definitions.begin(),
                                    m_definitions.end(),
                                    [&](const auto& def) { return def.name == name; });
    return found == m_definitions.end() ? nullptr : &*found;
}
Result Registry::Execute(const Definition& definition,
                         const std::vector<std::string>& arguments,
                         Context context) const {
    const auto required =
        static_cast<std::size_t>(std::count_if(definition.arguments.begin(),
                                               definition.arguments.end(),
                                               [](const auto& arg) { return !arg.optional; }));
    if (arguments.size() < required || arguments.size() > definition.arguments.size()) {
        return {Status::InvalidInput, "Invalid arity: " + definition.name};
    }
    std::vector<Value> values;
    values.reserve(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto result = Parse(definition.arguments[i], arguments[i], values);
        if (result.status != Status::Ok) {
            return result;
        }
    }
    if (definition.authority != Authority::Local || context.authority != Authority::Local ||
        (context.entry != EntryPoint::Console && context.entry != EntryPoint::Panel &&
         context.entry != EntryPoint::Shortcut && context.entry != EntryPoint::Headless)) {
        return {Status::Denied, "Authoritative execution or invalid context is unavailable"};
    }
    if (definition.available) {
        const auto availability = definition.available(definition.state);
        if (!availability.available) {
            return {Status::Unavailable,
                    availability.reason.empty() ? "Command unavailable"
                                                : availability.reason.substr(0, MaxInput)};
        }
    }
    return definition.execute(definition.state, {values, context});
}
Result Registry::Dispatch(std::string_view name,
                          const std::vector<std::string>& arguments,
                          Context context) {
    if (!m_sealed) {
        return {Status::Denied, "Command registry is not sealed"};
    }
    if (name.size() > MaxName || arguments.size() > MaxArguments || !ValidText(name)) {
        return {Status::InvalidInput, "Command input exceeds limits"};
    }
    std::size_t bytes = name.size();
    for (const auto& argument : arguments) {
        if (bytes == MaxInput || argument.size() > MaxInput - bytes - 1 || !ValidText(argument)) {
            return {Status::InvalidInput, "Command input exceeds limits"};
        }
        bytes += argument.size() + 1;
    }
    const auto* definition = Find(name);
    if (!definition) {
        return {Status::UnknownCommand, "Unknown command: " + std::string(name)};
    }
    auto result = Execute(*definition, arguments, context);
    if (result.message.size() > MaxInput) {
        result.message.resize(MaxInput);
    }
    if (m_history.size() == MaxHistory) {
        m_history.pop_front();
    }
    m_history.push_back({std::string(name), arguments, context, result});
    return result;
}
Result Registry::Submit(std::string_view line, Context context) {
    if (!ValidText(line)) {
        return {Status::InvalidInput, "Submit one command line of at most 4 KiB"};
    }
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false;
    bool started = false;
    bool escape = false;
    for (const char character : line) {
        if (escape) {
            if (character != '"' && character != '\\') {
                return {Status::InvalidInput, "Invalid escape"};
            }
            token += character;
            escape = false;
        } else if (character == '\\' && quoted) {
            escape = true;
        } else if (character == '"') {
            quoted = !quoted;
            started = true;
        } else if ((character == ' ' || character == '\t') && !quoted) {
            if (started) {
                if (tokens.size() == MaxArguments + 1) {
                    return {Status::InvalidInput, "Too many arguments"};
                }
                tokens.push_back(std::move(token));
                token.clear();
                started = false;
            }
        } else {
            token += character;
            started = true;
        }
    }
    if (quoted || escape) {
        return {Status::InvalidInput, "Unterminated quoted argument"};
    }
    if (started) {
        if (tokens.size() == MaxArguments + 1) {
            return {Status::InvalidInput, "Too many arguments"};
        }
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        return {};
    }
    return Dispatch(tokens.front(), {tokens.begin() + 1, tokens.end()}, context);
}
std::string Registry::Help(std::string_view name) const {
    const auto* definition = Find(name);
    if (!definition) {
        return {};
    }
    std::ostringstream out;
    out << definition->name;
    for (const auto& argument : definition->arguments) {
        out << ' ' << (argument.optional ? '[' : '<') << argument.name << ':';
        switch (argument.type) {
            case ArgumentType::Bool:
                out << "true|false";
                break;
            case ArgumentType::Float:
                out << "float " << argument.minimum << ".." << argument.maximum;
                break;
            case ArgumentType::Enum:
                for (const auto& choice : argument.choices) {
                    out << choice << '|';
                }
                break;
            case ArgumentType::String:
                out << "string";
                break;
        }
        out << (argument.optional ? ']' : '>');
    }
    out << " — " << definition->description;
    if (definition->authority == Authority::Authoritative) {
        out << " (unavailable: Authoritative execution is unavailable)";
    } else if (definition->available) {
        const auto availability = definition->available(definition->state);
        if (!availability.available) {
            out << " (unavailable: "
                << (availability.reason.empty() ? "Command unavailable"
                                                : availability.reason.substr(0, MaxInput))
                << ')';
        }
    }
    return out.str();
}
std::vector<std::string> Registry::Complete(std::string_view prefix) const {
    std::vector<std::string> matches;
    if (prefix.size() > MaxName) {
        return matches;
    }
    for (const auto& definition : m_definitions) {
        if (definition.name.starts_with(prefix)) {
            matches.push_back(definition.name);
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}
const std::deque<HistoryEntry>& Registry::History() const {
    return m_history;
}
} // namespace Luminumbra::Commands
