#include "ui/UiLayout.h"
#include "ui/UiLayoutLimits.h"

#include <RmlUi/Core/Box.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/Types.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra::Client::UI {
namespace {

std::string DirectText(Rml::Element& element, Detail::UiLayoutStringBudget& strings) {
    std::string result;
    if (const auto* text = dynamic_cast<const Rml::ElementText*>(&element)) {
        strings.Add(text->GetText());
        result = text->GetText();
    } else {
        const int children = element.GetNumChildren(true);
        if (children > static_cast<int>(kUiLayoutMaxElements))
            throw std::length_error("UI layout child budget exceeded");
        for (int index = 0; index < children; ++index) {
            if (const auto* child_text =
                    dynamic_cast<const Rml::ElementText*>(element.GetChild(index))) {
                strings.Add(child_text->GetText());
                result += child_text->GetText();
            }
        }
    }
    // ASCII whitespace only, independent of the process locale. Preserve inner whitespace.
    constexpr auto whitespace = " \t\n\r\f\v";
    const auto begin = result.find_first_not_of(whitespace);
    if (begin == std::string::npos)
        return {};
    const auto end = result.find_last_not_of(whitespace);
    return result.substr(begin, end - begin + 1);
}

UiLayoutElement CollectElementRecord(Rml::Element& element,
                                     const std::optional<std::size_t>& parent,
                                     std::size_t depth,
                                     Detail::UiLayoutStringBudget& strings) {
    strings.Add(element.GetId());
    strings.Add(element.GetTagName());
    const auto offset = element.GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto size = element.GetBox().GetSize(Rml::BoxArea::Border);
    UiLayoutElement record{
        .id = element.GetId(),
        .tag = element.GetTagName(),
        .text = DirectText(element, strings),
        .rect = {offset.x, offset.y, size.x, size.y},
        .clip = std::nullopt,
        .clip_masked = false,
        // RmlUi 6.1 keeps its effective stacking list private. Raw GetZIndex()
        // cannot rank nested contexts, positioned elements, floats or flex items.
        .z = std::nullopt,
        .depth = depth,
        .parent = parent,
    };
    Rml::Rectanglei clip;
    Rml::ClipMaskGeometryList masks;
    if (Rml::ElementUtilities::GetClippingRegion(&element, clip, &masks)) {
        record.clip = UiLayoutRect{static_cast<double>(clip.Left()),
                                   static_cast<double>(clip.Top()),
                                   static_cast<double>(clip.Width()),
                                   static_cast<double>(clip.Height())};
    }
    record.clip_masked = !masks.empty();
    return record;
}

void CollectDocument(Rml::Element& document,
                     std::size_t& visited,
                     Detail::UiLayoutStringBudget& strings,
                     UiLayout& layout) {
    struct Pending {
        Rml::Element* element;
        std::optional<std::size_t> parent;
        std::size_t depth;
    };
    // Bounded by kUiLayoutMaxElements, including queued siblings and hidden nodes.
    std::vector<Pending> pending{{.element = &document, .parent = std::nullopt, .depth = 0}};
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        if (node.depth > kUiLayoutMaxDepth || visited == kUiLayoutMaxElements)
            throw std::length_error("UI layout tree budget exceeded");
        ++visited;
        if (!node.element->IsVisible())
            continue; // Rml also prunes invisible subtrees from its stacking contexts.
        const int children = node.element->GetNumChildren(true);
        if (static_cast<std::size_t>(children) > kUiLayoutMaxElements - visited - pending.size())
            throw std::length_error("UI layout child budget exceeded");
        const auto own_index = layout.rml_elements.size();
        layout.rml_elements.push_back(
            CollectElementRecord(*node.element, node.parent, node.depth, strings));
        for (int index = children; index > 0; --index) {
            // RmlUi 6.1 GetChild(int) indexes all children, including non-DOM
            // children. Only GetNumChildren has an include_non_dom_elements flag.
            if (auto* child = node.element->GetChild(index - 1))
                pending.push_back({.element = child, .parent = own_index, .depth = node.depth + 1});
        }
    }
}

std::vector<const ImGuiWindow*> WindowDrawOrder(const ImGuiContext& context) {
    using ChildOrder = std::tuple<bool, bool, int>;
    struct OrderedWindow {
        const ImGuiWindow* window;
        std::tuple<int, int, std::size_t> root_order;
        std::vector<ChildOrder> ancestry;
    };
    // Pointer hashing is used only for lookup, never for output iteration.
    // All storage is bounded by 4096 windows and 256 ancestors per window.
    std::unordered_map<const ImGuiWindow*, std::size_t> positions;
    for (int index = 0; index < context.Windows.Size; ++index)
        positions.emplace(context.Windows[index], static_cast<std::size_t>(index));
    const auto* target = context.NavWindowingTarget;
    const auto* top = target && !(target->Flags & ImGuiWindowFlags_NoBringToFrontOnFocus)
                          ? target->RootWindow
                          : nullptr;
    const auto* list = target ? context.NavWindowingListWindow : nullptr;
    std::vector<OrderedWindow> ordered;
    ordered.reserve(positions.size());
    for (const auto* window : context.Windows) {
        OrderedWindow entry{.window = window, .root_order = {}, .ancestry = {}};
        const auto* root = window;
        while ((root->Flags & ImGuiWindowFlags_ChildWindow) && root->ParentWindow) {
            if (entry.ancestry.size() == kUiLayoutMaxDepth)
                throw std::length_error("UI layout window depth budget exceeded");
            entry.ancestry.emplace_back((root->Flags & ImGuiWindowFlags_Popup) != 0,
                                        (root->Flags & ImGuiWindowFlags_Tooltip) != 0,
                                        root->BeginOrderWithinParent);
            root = root->ParentWindow;
        }
        std::reverse(entry.ancestry.begin(), entry.ancestry.end());
        // Mirror ImGui 1.92.1 Render/EndFrame: parent before sorted children,
        // navigation roots last in their layer, then the tooltip layer on top.
        const int layer = (root->Flags & ImGuiWindowFlags_Tooltip) ? 1 : 0;
        const int navigation = root == top ? 1 : root == list ? 2 : 0;
        entry.root_order = {layer, navigation, positions.at(root)};
        ordered.push_back(std::move(entry));
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.root_order != right.root_order)
            return left.root_order < right.root_order;
        return left.ancestry < right.ancestry;
    });
    std::vector<const ImGuiWindow*> result;
    result.reserve(ordered.size());
    for (const auto& entry : ordered)
        result.push_back(entry.window);
    return result;
}

} // namespace

UiLayout CollectUiLayout(Rml::Context* rml,
                         const ImGuiContext* imgui,
                         const std::string& screen,
                         int width,
                         int height) {
    Detail::CheckUiLayoutDimensions(width, height);
    Detail::UiLayoutStringBudget strings;
    strings.Add(screen);
    UiLayout layout{.screen = screen,
                    .width = width,
                    .height = height,
                    .rml_elements = {},
                    .imgui_windows = {}};
    if (rml) {
        const int documents = rml->GetNumDocuments();
        if (documents > static_cast<int>(kUiLayoutMaxElements))
            throw std::length_error("UI layout document budget exceeded");
        std::size_t visited = 0;
        for (int index = 0; index < documents; ++index) {
            if (auto* document = rml->GetDocument(index))
                CollectDocument(*document, visited, strings, layout);
        }
    }
    if (imgui) {
        if (imgui->Windows.Size > static_cast<int>(kUiLayoutMaxWindows))
            throw std::length_error("UI layout window budget exceeded");
        const auto scale = imgui->IO.DisplayFramebufferScale;
        const auto origin = imgui->Viewports[0]->Pos;
        for (const auto* window : WindowDrawOrder(*imgui)) {
            strings.Add(window->Name);
            layout.imgui_windows.push_back({
                .title = window->Name,
                .rect = {(window->Pos.x - origin.x) * scale.x,
                         (window->Pos.y - origin.y) * scale.y,
                         window->Size.x * scale.x,
                         window->Size.y * scale.y},
                .collapsed = window->Collapsed,
                .visible = !window->Hidden,
            });
        }
    }
    return layout;
}

} // namespace Luminumbra::Client::UI
