#pragma once

#include <charconv>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace Luminumbra::Core {

// Parse one complete number without whitespace, allocation or locale dependence.
// Failure leaves the output unchanged. Floating from_chars requires GCC >= 11;
// the Ubuntu 24.04, MSVC and UCRT64 CI toolchains all provide it.
template<typename Number>
bool ParseNumber(std::string_view text, Number& output) {
    static_assert((std::is_integral_v<Number> && !std::is_same_v<Number, bool>) ||
                  std::is_floating_point_v<Number>);
    if (text.empty())
        return false;
    // Preserve the explicit positive sign accepted by the former sto* parsers.
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '+' || text.front() == '-')
            return false;
    }

    Number parsed{};
    std::from_chars_result result{};
    if constexpr (std::is_floating_point_v<Number>) {
        // sto* also accepted hexadecimal floats, including a negative sign.
        auto digits = text;
        const bool negative = digits.front() == '-';
        if (negative)
            digits.remove_prefix(1);
        const bool hex =
            digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
        if (hex) {
            digits.remove_prefix(2);
            if (digits.empty() || digits.front() == '+' || digits.front() == '-')
                return false;
            text = digits;
        }
        result = std::from_chars(text.data(),
                                 text.data() + text.size(),
                                 parsed,
                                 hex ? std::chars_format::hex : std::chars_format::general);
        if (hex && negative)
            parsed = -parsed;
    } else {
        result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    }
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    if constexpr (std::is_floating_point_v<Number>) {
        if (!std::isfinite(parsed))
            return false;
    }
    output = parsed;
    return true;
}

} // namespace Luminumbra::Core
