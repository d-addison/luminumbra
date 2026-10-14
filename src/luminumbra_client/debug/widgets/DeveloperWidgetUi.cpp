#include "debug/widgets/DeveloperWidgetUi.h"

#include <imgui.h>

namespace Luminumbra::Client::Debug {
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (passive) {
        flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
        ImGui::SetNextWindowPos(ImVec2(10, def.id == "perf.hud" ? 10 : 60), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
    } else {
        ImGui::SetNextWindowSize(def.id == "world.generation" ? ImVec2(550, 750) : ImVec2(480, 520),
                                 ImGuiCond_FirstUseEver);
    }
    const auto name = def.title + "###" + def.id;
    if (ImGui::Begin(name.c_str(), passive ? nullptr : &visible, flags)) {
        if (!passive && ImGui::IsWindowFocused())
            widgets.FocusInteraction(def.id);
        contents(frame);
    }
    ImGui::End();
    if (!visible)
        widgets.SetVisible(def.id, false);
}
} // namespace Luminumbra::Client::Debug
