#include "debug/widgets/RenderControlWidgets.h"

#include "rendering/passes/DebugViewPass.h"
#include <imgui.h>

namespace Luminumbra::Client::Debug {
void DrawDebugViewContents(DeveloperConsole& console, int mode) {
    const char* modes[] = {"off", "albedo", "normal", "depth", "material", "position"};
    if (ImGui::Combo("G-buffer view", &mode, modes, 6))
        console.Commands().Dispatch("render debug", {std::string(modes[mode])});
    if (ImGui::Button("Cycle view"))
        console.InvokeAction("render.debug-cycle");
    using Pass = Rendering::DebugViewPass;
    auto& control = Pass::controls();
    if (ImGui::Checkbox("Identity diagnostics", &control.enabled)) {
        control.refresh = control.enabled;
        control.result.clear();
        if (!control.enabled) {
            control.view = Pass::None;
            control.inspector_open = false;
        }
    }
    if (!control.enabled) {
        return;
    }
    int identity_mode = control.view == Pass::None ? 0 : control.view - Pass::DrawId + 1;
    if (ImGui::Combo("Identity snapshot view",
                     &identity_mode,
                     Pass::identity_mode_names.data(),
                     static_cast<int>(Pass::identity_mode_names.size()))) {
        control.view = identity_mode == 0 ? Pass::None : identity_mode + Pass::DrawId - 1;
        control.refresh = true;
    }
    ImGui::TextUnformatted("Magenta hatch: zero / unresolved / unsupported");
    ImGui::TextUnformatted(
        "Producer: terrain=brown, impostor=cyan, foliage=green, static=yellow, volume=blue");
    ImGui::TextUnformatted("Owner brightness: live=100%, legacy far=70%, ladder far=45%");
    ImGui::TextUnformatted(
        "Path: mesh=hashed, SDF=orange, heightfield=blue, unknown=magenta hatch");
    ImGui::Checkbox("Single-pixel inspector", &control.inspector_open);
    ImGui::Text("Render attachment: %u x %u (top-left texels)",
                control.extent.width,
                control.extent.height);
    if (control.inspector_open) {
        ImGui::InputInt("Pixel X", &control.x);
        ImGui::InputInt("Pixel Y", &control.y);
    }
    if (ImGui::Button("Refresh snapshot / inspect selected pixel")) {
        control.refresh = true;
    }
    ImGui::TextWrapped("%s", control.status.c_str());
    if (control.inspector_open) {
        ImGui::TextUnformatted(control.result.c_str());
        if (ImGui::Button("Copy pixel JSON")) {
            ImGui::SetClipboardText(control.result.c_str());
        }
    }
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
