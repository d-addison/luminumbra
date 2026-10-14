#include "ShaderSource.h"

#include <algorithm>
#include <string_view>

namespace Luminumbra::Rendering {
namespace {
bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
bool IsIdentifier(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
std::size_t LineAt(std::string_view source, std::size_t offset) {
    const auto prefix = source.substr(0, offset);
    return 1 + static_cast<std::size_t>(std::count(prefix.begin(), prefix.end(), '\n'));
}

// A minimal lexer: comments are trivia, and quoted directive operands cannot
// start comments. Return the end of a comment/string, or the next character.
bool Next(std::string_view source, std::size_t& cursor, std::string& diagnostic) {
    if (source.substr(cursor, 2) == "/*") {
        const auto end = source.find("*/", cursor + 2);
        if (end == std::string_view::npos) {
            diagnostic =
                "unterminated GLSL block comment at line " + std::to_string(LineAt(source, cursor));
            return false;
        }
        cursor = end + 2;
    } else if (source.substr(cursor, 2) == "//") {
        const auto end = source.find('\n', cursor + 2);
        cursor = end == std::string_view::npos ? source.size() : end;
    } else if (source[cursor] == '"') {
        ++cursor;
        while (cursor < source.size()) {
            const char c = source[cursor++];
            if (c == '"')
                break;
            if (c == '\\' && cursor < source.size())
                ++cursor;
        }
    } else {
        ++cursor;
    }
    return true;
}

bool SkipTrivia(std::string_view source, std::size_t& cursor, std::string& diagnostic) {
    while (cursor < source.size()) {
        if (IsSpace(source[cursor])) {
            ++cursor;
        } else if (source.substr(cursor, 2) == "//" || source.substr(cursor, 2) == "/*") {
            if (!Next(source, cursor, diagnostic))
                return false;
        } else {
            break;
        }
    }
    return true;
}
} // namespace

bool InjectShaderDefines(std::string& source,
                         std::span<const std::string> defines,
                         std::string& diagnostic) {
    if (defines.empty())
        return true;

    // Validate the entire source before mutation, including comments following
    // the first token. Never silently inject into an unclosed block comment.
    for (std::size_t cursor = 0; cursor < source.size();) {
        if (!Next(source, cursor, diagnostic))
            return false;
    }
    std::size_t token = 0;
    if (!SkipTrivia(source, token, diagnostic))
        return false;
    std::size_t insertion = 0;
    auto newline = source.find('\n');
    bool needs_newline = false;
    if (token < source.size() && source[token] == '#') {
        auto name = token + 1;
        if (!SkipTrivia(source, name, diagnostic))
            return false;
        constexpr std::string_view version = "version";
        if (LineAt(source, name) == LineAt(source, token) &&
            std::string_view(source).substr(name, version.size()) == version &&
            (name + version.size() == source.size() ||
             !IsIdentifier(source[name + version.size()]))) {
            const auto version_newline = source.find('\n', name);
            if (version_newline != std::string::npos)
                newline = version_newline;
            // Skip a trailing multiline block comment as a unit so the defines
            // cannot be swallowed. Stop at the next physical line outside it.
            insertion = name + version.size();
            while (insertion < source.size() && source[insertion] != '\n') {
                if (!Next(source, insertion, diagnostic))
                    return false;
            }
            if (insertion < source.size())
                ++insertion;
            else
                needs_newline = true;
        }
    }

    const std::string_view ending =
        newline != std::string::npos && newline > 0 && source[newline - 1] == '\r' ? "\r\n" : "\n";
    std::string injected;
    if (needs_newline)
        injected += ending;
    for (const auto& define : defines) {
        injected += "#define " + define;
        injected += ending;
    }
    const auto original_line = LineAt(source, insertion) + (needs_newline ? 1u : 0u);
    injected += "#line " + std::to_string(original_line);
    injected += ending;
    source.insert(insertion, injected);
    return true;
}
} // namespace Luminumbra::Rendering
