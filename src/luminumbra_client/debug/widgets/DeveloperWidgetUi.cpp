#include "debug/widgets/DeveloperWidgetUi.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luminumbra::Client::Debug {
void PrepareDeveloperPanel(const std::optional<DeveloperPanelLayout>& layout,
                           float width,
                           float height) {
    if (layout) {
        ImGui::SetNextWindowPos(ImVec2(layout->x, layout->y), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(layout->width, layout->height), ImGuiCond_Appearing);
    } else {
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
    }
}
std::optional<DeveloperPanelLayout> CaptureDeveloperPanel(bool open) {
    // Persist once a move/resize finishes, rather than write the overlay throughout a drag.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return std::nullopt;
    const auto position = ImGui::GetWindowPos();
    // Preserve the expanded size when a panel is collapsed.
    const auto size = ImGui::GetCurrentWindow()->SizeFull;
    return DeveloperPanelLayout{position.x, position.y, size.x, size.y, open};
}
void DrawDeveloperWidget(DeveloperWidgetRegistry& widgets,
                         DeveloperFrameAdapter& frame,
                         const WidgetDef& def,
                         const DrawFn& contents) {
    if (def.id == "world.creature_labels") {
        contents(frame); // Foreground labels have no window or input ownership.
        return;
    }
    const bool passive = def.presentation == Presentation::PassiveOverlay;
    bool visible = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (passive) {
        flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
        ImGui::SetNextWindowPos(ImVec2(10.0f, def.id == "perf.hud" ? 10.0f : 60.0f),
                                ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
    } else {
        PrepareDeveloperPanel(widgets.Layout(def.id),
                              def.id == "world.generation" ? 550.0f : 480.0f,
                              def.id == "world.generation" ? 750.0f : 520.0f);
    }
    const auto name = def.title + "###" + def.id;
    if (ImGui::Begin(name.c_str(), passive ? nullptr : &visible, flags)) {
        if (!passive && ImGui::IsWindowFocused())
            widgets.FocusInteraction(def.id);
        contents(frame);
    }
    const auto layout = passive ? std::nullopt : CaptureDeveloperPanel(visible);
    ImGui::End();
    if (!visible)
        widgets.SetVisible(def.id, false);
    if (layout)
        widgets.SetLayout(def.id, *layout);
}
} // namespace Luminumbra::Client::Debug
