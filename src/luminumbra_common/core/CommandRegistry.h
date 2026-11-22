#pragma once

#include <cstddef>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Luminumbra::Commands {

enum class ArgumentType {
    Bool,
    Float,
    Enum,
    String
};
using Value = std::variant<bool, float, std::string>;
enum class EntryPoint {
    Console,
    Panel,
    Shortcut,
    Headless
};
enum class Authority {
    Local,
    Authoritative
};
struct Context {
    EntryPoint entry = EntryPoint::Headless;
    Authority authority = Authority::Local;
    bool operator==(const Context&) const = default;
};
enum class Status {
    Ok,
    UnknownCommand,
    InvalidInput,
    Unavailable,
    Denied,
    Failed
};
struct Result {
    Status status = Status::Ok;
    std::string message = {};
    bool operator==(const Result&) const = default;
};
struct Availability {
    bool available = true;
    std::string reason;
};
struct Argument {
    std::string name;
    ArgumentType type = ArgumentType::String;
    bool optional = false;
    std::vector<std::string> choices = {};
    float minimum = 0;
    float maximum = 16;
};
struct Invocation {
    std::span<const Value> arguments;
    Context context;
};
struct Definition {
    std::string name;
    std::string description;
    std::vector<Argument> arguments;
    Authority authority = Authority::Local;
    // Borrowed host state must outlive the registry. Hooks run only on explicit
    // command/help requests, never on a frame or simulation tick.
    void* state = nullptr;
    Availability (*available)(void*) = nullptr;
    Result (*execute)(void*, const Invocation&) = nullptr;
};
struct HistoryEntry {
    std::string name;
    std::vector<std::string> arguments;
    Context context;
    Result result;
};

// Host-thread service; callbacks must not reenter or mutate this registry.
// Authoritative definitions are discoverable but cannot execute in this slice.
class Registry {
public:
    // All owned storage and scans are bounded: 256 definitions, 16 arguments,
    // 64 enum choices, 128-byte names and 4096-byte text/input/result payloads.
    // History retains at most 128 entries, each with <=4096 input + 4096 result bytes.
    static constexpr std::size_t MaxCommands = 256;
    static constexpr std::size_t MaxArguments = 16;
    static constexpr std::size_t MaxChoices = 64;
    static constexpr std::size_t MaxName = 128;
    static constexpr std::size_t MaxInput = 4096;
    static constexpr std::size_t MaxHistory = 128;

    bool Register(const Definition& definition);
    void Seal();
    Result Dispatch(std::string_view name,
                    const std::vector<std::string>& arguments = {},
                    Context context = {});
    Result Submit(std::string_view line, Context context = {});
    std::string Help(std::string_view name) const;
    std::vector<std::string> Complete(std::string_view prefix) const;
    // Oldest first. Only recognized dispatches with bounded input are retained,
    // including invalid/denied/unavailable results. Syntax errors are not retained.
    const std::deque<HistoryEntry>& History() const;

private:
    const Definition* Find(std::string_view name) const;
    Result Execute(const Definition& definition,
                   const std::vector<std::string>& arguments,
                   Context context) const;
    std::vector<Definition> m_definitions;
    std::deque<HistoryEntry> m_history;
    bool m_sealed = false;
};

} // namespace Luminumbra::Commands
