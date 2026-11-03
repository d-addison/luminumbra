#include "debug/widgets/RenderControlWidgets.h"

#include <imgui.h>

namespace Luminumbra::Client::Debug {
void DrawDebugViewContents(DeveloperConsole& console, int mode) {
    const char* modes[] = {"off", "albedo", "normal", "depth", "material", "position"};
    if (ImGui::Combo("G-buffer view", &mode, modes, 6))
        console.Commands().Dispatch("render debug", {std::string(modes[mode])});
    if (ImGui::Button("Cycle view"))
        console.InvokeAction("render.debug-cycle");
}
void DrawWireframeContents(DeveloperConsole& console, bool enabled) {
    if (ImGui::Checkbox("Wireframe effect", &enabled))
        console.Commands().Dispatch("render wireframe", {enabled ? "true" : "false"});
}
void DrawTimeControlContents(DeveloperConsole& console, float scale) {
    if (ImGui::SliderFloat("Time scale", &scale, 0.0f, 16.0f, "%.2fx"))
        console.Commands().Dispatch("time scale", {std::to_string(scale)});
    if (ImGui::Button("Slower"))
        console.InvokeAction("time.slower");
    ImGui::SameLine();
    if (ImGui::Button("Faster"))
        console.InvokeAction("time.faster");
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
        console.InvokeAction("time.reset");
}
} // namespace Luminumbra::Client::Debug
