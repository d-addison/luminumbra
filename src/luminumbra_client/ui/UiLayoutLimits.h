#pragma once

#include "ui/UiLayout.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace Luminumbra::Client::UI::Detail {

// Aggregate source string bytes, before JSON escaping (at most six output bytes
// per input byte). Subtract before adding, including on 32-bit platforms.
class UiLayoutStringBudget {
public:
    void Add(std::string_view value) {
        if (value.size() > kUiLayoutMaxStringBytes - m_used)
            throw std::length_error("UI layout string budget exceeded");
        m_used += value.size();
    }

private:
    std::size_t m_used = 0;
};

inline void CheckUiLayoutDimensions(int width, int height) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("UI layout requires a drawable framebuffer");
}

} // namespace Luminumbra::Client::UI::Detail
