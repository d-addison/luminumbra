#include "debug/DeveloperConsole.h"
#include "debug/widgets/DeveloperWidgetUi.h"

#include <imgui.h>

namespace Luminumbra::Client::Debug {
void DeveloperConsole::Draw(DeveloperFrameAdapter& frame) {
    m_widgets.DrawVisible(frame, Presentation::PassiveOverlay);
    if (m_open) {
        if (m_refreshInventory) {
            for (const auto& def : m_widgets.List())
                m_inventoryAvailability[def.id] = def.available ? def.available() : Availability{};
            m_refreshInventory = false;
        }
        PrepareDeveloperPanel(Layout(), 820, 650);
        bool open = m_open;
        if (ImGui::Begin("Developer console", &open, ImGuiWindowFlags_NoSavedSettings)) {
            if (ImGui::IsWindowFocused())
                m_shellSequence = m_widgets.NextInteractionSequence();
            if (ImGui::SmallButton("Refresh services"))
                RefreshAvailability();
            if (m_dirty && ImGui::Button("Retry saving preferences"))
                SavePreferences();
            if (IsCapturing())
                ImGui::Text("Binding %s: press a key (Escape cancels)", m_capture.c_str());
            ImGui::InputText("Search widgets", m_search.data(), m_search.size());
            if (ImGui::BeginChild("Inventory", ImVec2(0, 230), true)) {
                for (const auto& def : m_widgets.List()) {
                    const std::string search = m_search.data();
                    if (!search.empty() &&
                        (def.id + def.title + def.category + def.description).find(search) ==
                            std::string::npos)
                        continue;
                    ImGui::PushID(def.id.c_str());
                    const auto& available = m_inventoryAvailability.at(def.id);
                    const bool action_only = def.presentation == Presentation::ActionOnly;
                    ImGui::Text("%s / %s — %s",
                                def.category.c_str(),
                                def.title.c_str(),
                                action_only                 ? "action only"
                                : m_widgets.Visible(def.id) ? "shown"
                                                            : "hidden");
                    ImGui::TextWrapped("%s", def.description.c_str());
                    if (!available.available)
                        ImGui::TextDisabled("Unavailable: %s", available.reason.c_str());
                    if (!action_only) {
                        if (ImGui::SmallButton("Toggle"))
                            RecordResult(m_commands.Dispatch("widget toggle", {def.id}));
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Show"))
                            RecordResult(m_commands.Dispatch("widget show", {def.id}));
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Hide"))
                            RecordResult(m_commands.Dispatch("widget hide", {def.id}));
                    }
                    for (const auto& binding : def.BindingDefinitions()) {
                        ImGui::PushID(binding.id.c_str());
                        ImGui::Text("%s: %s",
                                    binding.id.c_str(),
                                    FormatKeyChord(m_bindings.Get(binding.id)).c_str());
                        if (!def.shortcuts.empty()) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Run"))
                                InvokeAction(binding.id);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Rebind"))
                            BeginRebind(binding.id);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Unbind"))
                            RecordResult(m_commands.Dispatch("unbind", {binding.id}));
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            if (ImGui::CollapsingHeader("Actions and resolved bindings")) {
                for (const auto& def : m_bindings.List()) {
                    bool module_binding = false;
                    for (const auto& widget : m_widgets.List())
                        for (const auto& binding : widget.BindingDefinitions())
                            module_binding = module_binding || binding.id == def.id;
                    if (module_binding)
                        continue;
                    ImGui::PushID(def.id.c_str());
                    ImGui::Text(
                        "%s: %s", def.id.c_str(), FormatKeyChord(m_bindings.Get(def.id)).c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Rebind"))
                        BeginRebind(def.id);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Unbind"))
                        RecordResult(m_commands.Dispatch("unbind", {def.id}));
                    ImGui::PopID();
                }
            }
            if (ImGui::SmallButton("Help"))
                Submit("help");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear log"))
                Submit("log clear");
            if (ImGui::BeginChild("Log", ImVec2(0, -60), true)) {
                const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
                const auto records = m_log->Snapshot(m_minimum);
                for (const auto& record : records) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        record.timestamp.time_since_epoch())
                                        .count();
                    ImGui::Text("%llu / %lld / %d",
                                static_cast<unsigned long long>(record.sequence),
                                static_cast<long long>(ms),
                                static_cast<int>(record.severity));
                    ImGui::TextUnformatted(record.message.c_str());
                }
                if (follow)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            auto history_callback = [](ImGuiInputTextCallbackData* data) {
                auto* console = static_cast<DeveloperConsole*>(data->UserData);
                const auto line = data->EventKey == ImGuiKey_UpArrow
                                      ? console->m_history.Up(data->Buf)
                                      : console->m_history.Down();
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, line.c_str());
                return 0;
            };
            if (m_focusInput) {
                ImGui::SetKeyboardFocusHere();
                m_focusInput = false;
            }
            if (ImGui::InputText("Command",
                                 m_input.data(),
                                 m_input.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_CallbackHistory,
                                 history_callback,
                                 this)) {
                Submit(m_input.data());
                m_input[0] = '\0';
                ImGui::SetKeyboardFocusHere(-1);
            }
        }
        const auto layout = CaptureDeveloperPanel(open);
        ImGui::End();
        if (open != m_open)
            SetOpen(open);
        if (layout)
            SetLayout(*layout);
    }
    DeveloperFrameAdapter panels = frame;
    panels.draw = [this, draw = frame.draw](const WidgetDef& def, const DrawFn& contents) {
        if (!draw)
            return;
        if (def.id == "console.help") {
            draw(def, [this](DeveloperFrameAdapter&) {
                const auto help = HelpText();
                ImGui::TextUnformatted(help.c_str());
            });
        } else
            draw(def, contents);
    };
    m_widgets.DrawVisible(panels, Presentation::ConsolePanel);
}
} // namespace Luminumbra::Client::Debug
