#include "ui/UiLayout.h"
#include "ui/UiLayoutLimits.h"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <locale>
#include <nlohmann/json.hpp>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Luminumbra::Client::UI {
namespace {
using LayoutJson = nlohmann::basic_json<>;

void WriteRect(std::ostream& output, const UiLayoutRect& rect) {
    output << "{\"x\":" << rect.x << ",\"y\":" << rect.y << ",\"w\":" << rect.w
           << ",\"h\":" << rect.h << '}';
}

void CheckRect(const UiLayoutRect& rect) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) ||
        !std::isfinite(rect.h) || rect.w < 0 || rect.h < 0)
        throw std::invalid_argument("UI layout geometry must be finite with nonnegative sizes");
}

void ValidateLayout(const UiLayout& layout) {
    Detail::CheckUiLayoutDimensions(layout.width, layout.height);
    if (layout.rml_elements.size() > kUiLayoutMaxElements ||
        layout.imgui_windows.size() > kUiLayoutMaxWindows)
        throw std::length_error("UI layout item budget exceeded");
    Detail::UiLayoutStringBudget strings;
    strings.Add(layout.screen);
    for (std::size_t index = 0; index < layout.rml_elements.size(); ++index) {
        const auto& element = layout.rml_elements[index];
        strings.Add(element.id);
        strings.Add(element.tag);
        strings.Add(element.text);
        CheckRect(element.rect);
        if (element.clip)
            CheckRect(*element.clip);
        if (element.depth > kUiLayoutMaxDepth)
            throw std::length_error("UI layout depth budget exceeded");
        if (element.parent) {
            if (*element.parent >= index ||
                element.depth != layout.rml_elements[*element.parent].depth + 1)
                throw std::invalid_argument("UI layout parent must precede its child");
        } else if (element.depth != 0) {
            throw std::invalid_argument("UI layout root depth must be zero");
        }
    }
    for (const auto& window : layout.imgui_windows) {
        strings.Add(window.title);
        CheckRect(window.rect);
    }
}

} // namespace

std::string SerializeUiLayout(const UiLayout& layout) {
    ValidateLayout(layout);
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(3) << std::boolalpha;
    output << "{\"schema\":1,\"screen\":" << LayoutJson(layout.screen).dump()
           << ",\"width\":" << layout.width << ",\"height\":" << layout.height
           << ",\"rml_elements\":[";
    for (std::size_t index = 0; index < layout.rml_elements.size(); ++index) {
        const auto& element = layout.rml_elements[index];
        if (index != 0)
            output << ',';
        output << "{\"id\":" << LayoutJson(element.id).dump()
               << ",\"tag\":" << LayoutJson(element.tag).dump()
               << ",\"text\":" << LayoutJson(element.text).dump() << ",\"rect\":";
        WriteRect(output, element.rect);
        output << ",\"clip\":";
        if (element.clip)
            WriteRect(output, *element.clip);
        else
            output << "null";
        output << ",\"clip_masked\":" << element.clip_masked << ",\"z\":";
        if (element.z)
            output << *element.z;
        else
            output << "null";
        output << ",\"depth\":" << element.depth << ",\"parent\":";
        if (element.parent)
            output << *element.parent;
        else
            output << "null";
        output << '}';
    }
    output << "],\"imgui_windows\":[";
    for (std::size_t index = 0; index < layout.imgui_windows.size(); ++index) {
        const auto& window = layout.imgui_windows[index];
        if (index != 0)
            output << ',';
        output << "{\"title\":" << LayoutJson(window.title).dump() << ",\"rect\":";
        WriteRect(output, window.rect);
        output << ",\"collapsed\":" << window.collapsed << ",\"visible\":" << window.visible << '}';
    }
    output << "]}\n";
    if (!output)
        throw std::runtime_error("UI layout serialization failed");
    return output.str();
}

} // namespace Luminumbra::Client::UI
