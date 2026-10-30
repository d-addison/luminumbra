#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
}
struct ImGuiContext;

namespace Luminumbra::Client::UI {

struct UiLayoutRect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
};

struct UiLayoutElement {
    std::string id;
    std::string tag;
    std::string text;
    UiLayoutRect rect;
    std::optional<UiLayoutRect> clip;
    bool clip_masked = false;
    std::optional<std::int64_t> z;
    std::size_t depth = 0;
    std::optional<std::size_t> parent;
};

struct UiLayoutWindow {
    std::string title;
    UiLayoutRect rect;
    bool collapsed = false;
    bool visible = false;
};

struct UiLayout {
    std::string screen;
    int width = 0;
    int height = 0;
    std::vector<UiLayoutElement> rml_elements;
    std::vector<UiLayoutWindow> imgui_windows;
};

// Capture-only limits: refuse rather than silently truncate a diagnostic snapshot.
inline constexpr std::size_t kUiLayoutMaxElements = 65'536;
inline constexpr std::size_t kUiLayoutMaxWindows = 4'096;
inline constexpr std::size_t kUiLayoutMaxDepth = 256;
inline constexpr std::size_t kUiLayoutMaxStringBytes = std::size_t{16} * 1024 * 1024;

// Call on the UI thread after Rml rendering, while supplied contexts remain alive.
// Supply only contexts whose UI is in the paired image. Menu CLI captures precede
// the ImGui composite and pass a null ImGui context, yielding no ImGui windows.
// Visible Rml subtrees include non-DOM children and their immediate text nodes.
// Null contexts produce empty arrays. No GL calls, simulation writes or event dispatch.
// Throws on invalid dimensions or capture limits. Rml may refresh diagnostic caches.
UiLayout CollectUiLayout(
    Rml::Context* rml, const ImGuiContext* imgui, const std::string& screen, int width, int height);

// GL-free schema-1 JSON, fixed key order and three decimal places, with a final LF.
// Throws on invalid dimensions, non-finite geometry, broken parent links or limits.
std::string SerializeUiLayout(const UiLayout& layout);

} // namespace Luminumbra::Client::UI
