#pragma once

#include "debug/DeveloperCommandRegistry.h"
#include "debug/DeveloperLogSink.h"

#include <array>

namespace Luminumbra::Client::Debug {
class DeveloperHistory {
public:
    static constexpr std::size_t kLimit = 128;
    void Submit(const std::string& line);
    std::string Up(const std::string& draft);
    std::string Down();
    const std::vector<std::string>& Entries() const;

private:
    std::vector<std::string> m_entries;
    std::size_t m_position = 0;
    std::string m_draft;
};
struct DeveloperHost {
    AvailabilityFn renderer_available;
    // The host queues render work; callbacks resolve current services on invocation.
    std::function<CommandResult(const std::string&, const CommandArguments&)> execute;
    std::function<std::vector<std::string>()> shader_names;
    std::function<bool()> save;
    std::function<std::string()> gameplay_help;
};
// Only integration translation units complete the adapter. Pure tests may use an empty adapter.
struct DeveloperFrameAdapter {
    std::function<void(const WidgetDef&, const DrawFn&)> draw;
};
class DeveloperConsole {
public:
    DeveloperConsole();
    void Initialize(DeveloperHost host, std::vector<WidgetDef> widgets = BuiltinDeveloperWidgets());
    DeveloperBindings& Bindings();
    DeveloperWidgetRegistry& Widgets();
    DeveloperCommandRegistry& Commands();
    std::shared_ptr<DeveloperLogSink> Log() const;
    void SetOpen(bool open);
    bool IsOpen() const;
    std::optional<DeveloperPanelLayout> Layout() const;
    void SetLayout(DeveloperPanelLayout layout);
    void RestoreLayout(DeveloperPanelLayout layout);
    bool HasInteraction() const;
    bool CloseTopInteraction();
    void BeginRebind(const std::string& action);
    bool IsCapturing() const;
    void CaptureKey(int key, int mods);
    CommandResult Submit(const std::string& line);
    CommandResult InvokeAction(const std::string& action);
    void PreferencesChanged();
    bool SavePreferences();
    bool Dirty() const;
    std::string HelpText() const;
    void RefreshAvailability();
    // Call after the UI override gate. Hidden shell does not snapshot or format logs.
    void Draw(DeveloperFrameAdapter& frame);
    void Shutdown();

private:
    void RegisterCommands();
    std::string WidgetHelp() const;
    std::string BindingHelp() const;
    void RecordResult(const CommandResult& result);
    DeveloperBindings m_bindings;
    DeveloperWidgetRegistry m_widgets;
    DeveloperCommandRegistry m_commands;
    DeveloperHistory m_history;
    std::shared_ptr<DeveloperLogSink> m_log;
    DeveloperHost m_host;
    std::uint64_t m_shellSequence = 0;
    bool m_focusInput = false;
    bool m_open = false;
    std::optional<DeveloperPanelLayout> m_layout;
    bool m_refreshInventory = true;
    std::map<std::string, Availability> m_inventoryAvailability;
    mutable bool m_refreshHelp = true;
    mutable std::string m_cachedHelp;
    bool m_dirty = false;
    bool m_batch_preferences = false;
    bool m_initialized = false;
    std::string m_capture;
    LogSeverity m_minimum = LogSeverity::Trace;
    std::array<char, DeveloperCommandRegistry::kInputLimit + 1> m_input{};
    std::array<char, 128> m_search{};
};
} // namespace Luminumbra::Client::Debug
