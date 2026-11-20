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
            control.armed = false;
            control.frozen = false;
            control.inspect_requested = false;
            control.probe_json.clear();
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
    if (ImGui::Checkbox("Single-pixel inspector", &control.inspector_open) &&
        !control.inspector_open) {
        control.armed = false;
        control.inspect_requested = false;
        control.zoom_texture = 0;
    }
    ImGui::Text("Render attachment: %u x %u (top-left texels)",
                control.extent.width,
                control.extent.height);
    ImGui::Text("Output resolution: %u x %u (framebuffer pixels)",
                control.output.width,
                control.output.height);
    if (control.inspector_open) {
        ImGui::Checkbox("Arm viewport picking", &control.armed);
        ImGui::InputInt("Pixel X", &control.x);
        ImGui::InputInt("Pixel Y", &control.y);
    }
    if (ImGui::Button("Refresh snapshot / inspect selected pixel")) {
        control.frozen = false;
        control.refresh = true;
        Pass::request_inspection(control);
    }
    if (control.inspector_open && ImGui::Checkbox("Freeze frame", &control.frozen) &&
        control.frozen && control.image_texture == 0) {
        control.refresh = true;
    }
    if (control.frozen && control.image_texture != 0 && control.extent.width != 0) {
        ImGui::TextUnformatted("Frozen deferred albedo (image, camera and draw table)");
        ImGui::Image(static_cast<ImTextureID>(control.image_texture),
                     ImVec2(256,
                            256.0f * static_cast<float>(control.extent.height) /
                                static_cast<float>(control.extent.width)),
                     ImVec2(0, 1),
                     ImVec2(1, 0));
    }
    ImGui::TextWrapped("%s", control.status.c_str());
    if (control.inspector_open) {
        ImGui::TextUnformatted(control.result.c_str());
        if (ImGui::Button("Inspect selected pixel")) {
            Pass::request_inspection(control);
        }
        if (control.zoom_texture != 0) {
            ImGui::TextUnformatted("9 x 9 nearest texels: linear deferred albedo (edges clamped)");
            ImGui::Image(static_cast<ImTextureID>(control.zoom_texture),
                         ImVec2(216, 216),
                         ImVec2(0, 1),
                         ImVec2(1, 0));
        }
        ImGui::BeginDisabled(control.probe_json.empty());
        if (ImGui::Button("Copy pixel JSON")) {
            ImGui::SetClipboardText(control.probe_json.c_str());
        }
        ImGui::EndDisabled();
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
