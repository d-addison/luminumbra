#include "debug/DeveloperConsole.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Luminumbra::Client::Debug {
namespace {
bool Blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
}
const std::string& Text(const CommandArguments& args, std::size_t i) {
    return std::get<std::string>(args[i]);
}
} // namespace
void DeveloperHistory::Submit(const std::string& line) {
    if (Blank(line) || line.size() > DeveloperCommandRegistry::kInputLimit)
        return;
    if (m_entries.empty() || m_entries.back() != line)
        m_entries.push_back(line);
    if (m_entries.size() > kLimit)
        m_entries.erase(m_entries.begin());
    m_position = m_entries.size();
    m_draft.clear();
}
std::string DeveloperHistory::Up(const std::string& draft) {
    if (m_position == m_entries.size())
        m_draft = draft;
    if (m_position)
        --m_position;
    return m_position < m_entries.size() ? m_entries[m_position] : m_draft;
}
std::string DeveloperHistory::Down() {
    if (m_position < m_entries.size())
        ++m_position;
    return m_position < m_entries.size() ? m_entries[m_position] : m_draft;
}
const std::vector<std::string>& DeveloperHistory::Entries() const {
    return m_entries;
}
DeveloperConsole::DeveloperConsole()
    : m_widgets(m_bindings)
    , m_commands(m_widgets, m_bindings)
    , m_log(std::make_shared<DeveloperLogSink>()) {}
void DeveloperConsole::Initialize(DeveloperHost host, std::vector<WidgetDef> widgets) {
    if (m_initialized)
        return;
    m_initialized = true;
    m_host = std::move(host);
    for (auto& def : widgets) {
        if (def.id == "console.help") {
            def.available = [] {
                return Availability{};
            };
            def.draw = [](DeveloperFrameAdapter& frame) {
                if (frame.draw) {
                    WidgetDef help;
                    help.id = "console.help";
                    frame.draw(help, {});
                }
            };
        }
        m_widgets.Register(std::move(def));
    }
    m_bindings.conflict_reported = [this](const std::string& message) {
        m_log->Push(LogSeverity::Warning, message);
    };
    RegisterCommands();
    m_widgets.Seal();
    m_commands.Seal();
    m_widgets.changed = [this] {
        PreferencesChanged();
    };
    m_bindings.changed = [this] {
        PreferencesChanged();
    };
}
DeveloperBindings& DeveloperConsole::Bindings() {
    return m_bindings;
}
DeveloperWidgetRegistry& DeveloperConsole::Widgets() {
    return m_widgets;
}
DeveloperCommandRegistry& DeveloperConsole::Commands() {
    return m_commands;
}
std::shared_ptr<DeveloperLogSink> DeveloperConsole::Log() const {
    return m_log;
}
void DeveloperConsole::SetOpen(bool open) {
    if (open && !m_open) {
        m_shellSequence = m_widgets.NextInteractionSequence();
        m_focusInput = true;
    }
    m_open = open;
    if (open)
        RefreshAvailability();
}
bool DeveloperConsole::IsOpen() const {
    return m_open;
}
bool DeveloperConsole::HasInteraction() const {
    return m_open || IsCapturing() || m_widgets.HasInteractivePanel();
}
bool DeveloperConsole::CloseTopInteraction() {
    if (IsCapturing()) {
        m_capture.clear();
        return true;
    }
    if (m_open && m_shellSequence > m_widgets.TopInteractionSequence()) {
        SetOpen(false);
        return true;
    }
    if (m_widgets.CloseTopInteraction())
        return true;
    if (m_open) {
        SetOpen(false);
        return true;
    }
    return false;
}
void DeveloperConsole::BeginRebind(const std::string& action) {
    if (m_bindings.Contains(action))
        m_capture = action;
}
bool DeveloperConsole::IsCapturing() const {
    return !m_capture.empty();
}
void DeveloperConsole::CaptureKey(int key, int mods) {
    if (!IsCapturing())
        return;
    if (key == 256) {
        m_capture.clear();
        return;
    }
    if (IsModifierKey(key))
        return;
    const auto action = std::move(m_capture);
    m_capture.clear();
    RecordResult(m_commands.Dispatch("bind", {action, FormatKeyChord(KeyChord{key, mods & 15})}));
}
void DeveloperConsole::RecordResult(const CommandResult& result) {
    if (!result.message.empty())
        m_log->Push(result.success ? LogSeverity::Info : LogSeverity::Error, result.message);
}
CommandResult DeveloperConsole::Submit(const std::string& line) {
    if (Blank(line))
        return {};
    if (line.size() <= DeveloperCommandRegistry::kInputLimit)
        m_log->Push(LogSeverity::Info, "> " + line);
    m_history.Submit(line);
    const auto result = m_commands.Submit(line);
    RecordResult(result);
    return result;
}
CommandResult DeveloperConsole::InvokeAction(const std::string& action) {
    const auto result = m_commands.InvokeAction(action);
    RecordResult(result);
    return result;
}
void DeveloperConsole::PreferencesChanged() {
    RefreshAvailability();
    m_dirty = true;
    if (!m_batch_preferences)
        SavePreferences();
}
bool DeveloperConsole::SavePreferences() {
    if (!m_host.save || !m_host.save()) {
        m_dirty = true;
        RecordResult({false, "Developer preferences save failed; use settings save to retry"});
        return false;
    }
    m_dirty = false;
    return true;
}
void DeveloperConsole::RefreshAvailability() {
    m_refreshInventory = true;
    m_refreshHelp = true;
}
std::string DeveloperConsole::HelpText() const {
    if (m_refreshHelp) {
        m_cachedHelp = m_commands.Help() + WidgetHelp() + BindingHelp();
        m_refreshHelp = false;
    }
    return m_cachedHelp;
}
bool DeveloperConsole::Dirty() const {
    return m_dirty;
}
void DeveloperConsole::Shutdown() {
    m_widgets.Shutdown();
    m_bindings.changed = {};
    m_bindings.external_conflict = {};
    m_bindings.conflict_reported = {};
    m_host = {};
    m_log->Close();
}
std::string DeveloperConsole::WidgetHelp() const {
    std::ostringstream out;
    for (const auto& def : m_widgets.List()) {
        out << def.id << " — " << def.title << " ("
            << (m_widgets.Visible(def.id) ? "shown" : "hidden") << ") "
            << FormatKeyChord(m_bindings.Get(def.id));
        if (def.available) {
            const auto a = def.available();
            if (!a.available)
                out << " — unavailable: " << a.reason;
        }
        out << '\n';
    }
    return out.str();
}
std::string DeveloperConsole::BindingHelp() const {
    std::ostringstream out;
    for (const auto& def : m_bindings.List())
        out << def.id << " = " << FormatKeyChord(m_bindings.Get(def.id)) << '\n';
    if (m_host.gameplay_help)
        out << m_host.gameplay_help();
    return out.str();
}
void DeveloperConsole::RegisterCommands() {
    auto command = [&](std::string name,
                       std::string description,
                       std::vector<CommandArgument> args,
                       auto execute,
                       AvailabilityFn available = {}) {
        m_commands.Register({std::move(name),
                             std::move(description),
                             std::move(args),
                             std::move(available),
                             execute});
    };
    command("help",
            "Generated command syntax, availability and bindings",
            {{"command", ArgumentType::Enum, true, {}}},
            [this](const CommandArguments& args) {
                const auto help = m_commands.Help(args.empty() ? "" : Text(args, 0));
                return CommandResult{!help.empty(),
                                     help.empty() ? "Unknown command"
                                                  : help + (args.empty() ? BindingHelp() : "")};
            });
    command("widgets", "List widget inventory", {}, [this](const CommandArguments&) {
        return CommandResult{true, WidgetHelp()};
    });
    for (const std::string operation : {"show", "hide", "toggle"}) {
        command("widget " + operation,
                "Change widget visibility",
                {{"id", ArgumentType::WidgetId}},
                [this, operation](const CommandArguments& args) {
                    const auto& id = Text(args, 0);
                    const bool changed = operation == "toggle"
                                             ? m_widgets.Toggle(id)
                                             : m_widgets.SetVisible(id, operation == "show");
                    const auto* def = m_widgets.Find(id);
                    const auto available =
                        def && def->available ? def->available() : Availability{};
                    return CommandResult{
                        changed,
                        available.available
                            ? ""
                            : "Visibility preference updated; presentation unavailable: " +
                                  available.reason};
                });
    }
    command("bind",
            "Rebind a widget or action",
            {{"action", ArgumentType::ActionId}, {"chord", ArgumentType::KeyChord}},
            [this](const CommandArguments& args) {
                std::string error;
                const bool ok =
                    m_bindings.Rebind(Text(args, 0), std::get<KeyChord>(args[1]), error);
                return CommandResult{ok, error};
            });
    command("unbind",
            "Remove an action binding",
            {{"action", ArgumentType::ActionId}},
            [this](const CommandArguments& args) {
                std::string error;
                const bool ok = m_bindings.Rebind(Text(args, 0), std::nullopt, error);
                return CommandResult{ok, error};
            });
    command("bindings",
            "Show resolved developer and gameplay bindings",
            {},
            [this](const CommandArguments&) { return CommandResult{true, BindingHelp()}; });
    command("console toggle",
            "Open or close the developer console",
            {},
            [this](const CommandArguments&) {
                SetOpen(!m_open);
                return CommandResult{};
            });
    auto host_command = [&](std::string name,
                            std::string description,
                            std::vector<CommandArgument> args,
                            bool renderer) {
        command(
            name,
            description,
            std::move(args),
            [this, name](const CommandArguments& values) {
                if (!m_host.execute)
                    return CommandResult{false, "Client service unavailable"};
                if (name == "shader reload" && !values.empty() && Text(values, 0) != "all") {
                    const auto names =
                        m_host.shader_names ? m_host.shader_names() : std::vector<std::string>{};
                    if (std::find(names.begin(), names.end(), Text(values, 0)) == names.end())
                        return CommandResult{false, "Unknown shader: " + Text(values, 0)};
                }
                return m_host.execute(name, values);
            },
            renderer ? AvailabilityFn([this] {
                return m_host.renderer_available ? m_host.renderer_available()
                                                 : Availability{false, "Renderer unavailable"};
            })
                     : AvailabilityFn{});
    };
    host_command("shader reload",
                 "Queue rollback-safe shader reload",
                 {{"all|name", ArgumentType::Enum, true, {}}},
                 true);
    host_command("shader watch",
                 "Enable independent shader watcher",
                 {{"enabled", ArgumentType::Bool}},
                 true);
    host_command("render debug",
                 "Select G-buffer view",
                 {{"mode",
                   ArgumentType::Enum,
                   false,
                   {"off", "albedo", "normal", "depth", "material", "position"}}},
                 true);
    host_command("render debug-cycle", "Cycle G-buffer views", {}, true);
    host_command(
        "render wireframe", "Set wireframe effect", {{"enabled", ArgumentType::Bool}}, true);
    host_command("render wireframe-toggle", "Toggle wireframe effect", {}, true);
    host_command(
        "time scale", "Set runtime time scale (0–16)", {{"value", ArgumentType::Float}}, false);
    for (const std::string op : {"slower", "faster", "reset"})
        host_command("time " + op, "Adjust runtime time scale", {}, false);
    command("settings save",
            "Save user preferences; retry failed save",
            {},
            [this](const CommandArguments&) { return CommandResult{SavePreferences(), {}}; });
    command("settings defaults developer",
            "Reset only developer visibility and bindings",
            {},
            [this](const CommandArguments&) {
                m_batch_preferences = true;
                m_widgets.RestoreDefaults();
                m_batch_preferences = false;
                return CommandResult{SavePreferences(), {}};
            });
    command("log clear", "Clear console records", {}, [this](const CommandArguments&) {
        m_log->Clear();
        return CommandResult{};
    });
    command("log level",
            "Filter console display only",
            {{"level",
              ArgumentType::Enum,
              false,
              {"trace", "debug", "info", "warning", "error", "critical"}}},
            [this](const CommandArguments& args) {
                const std::vector<std::string> levels{
                    "trace", "debug", "info", "warning", "error", "critical"};
                m_minimum = static_cast<LogSeverity>(
                    std::find(levels.begin(), levels.end(), Text(args, 0)) - levels.begin());
                return CommandResult{};
            });
    auto action = [&](const char* id,
                      int key,
                      const char* name,
                      unsigned scopes = Global | Gameplay | Photo) {
        m_bindings.Register({id, KeyChord{key, 0}, scopes});
        m_commands.BindAction(id, name);
    };
    action("console.toggle", 290, "console toggle");
    action("shader.reload", 294, "shader reload");
    action("render.debug-cycle", 295, "render debug-cycle");
    action("render.wireframe-toggle", 298, "render wireframe-toggle");
    action("time.slower", 91, "time slower", Gameplay);
    action("time.faster", 93, "time faster", Gameplay);
    action("time.reset", 92, "time reset", Gameplay | Photo);
    for (const auto& def : m_widgets.List())
        m_commands.BindAction(def.id, "widget toggle", {def.id});
}
} // namespace Luminumbra::Client::Debug
