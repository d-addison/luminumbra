#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>

namespace Luminumbra::Rendering {

inline constexpr int kDefaultVolumetricQuality = 1;
inline constexpr float kDefaultAetherMaterialModulation = 1.0f;

inline bool ScentDecalEnabled(const std::optional<std::string>& environment) {
    return environment.value_or("1") != "0";
}

namespace Detail {

inline bool OverrideSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline int OverrideDigit(char c, bool hex = false) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (hex && c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (hex && c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline bool ParseOverrideNumber(const char*& cursor, int& value) {
    // %d is decimal, including leading zeroes. Unlike scanf's undefined
    // out-of-range integer assignment, reject values that cannot fit in int.
    const char* begin = cursor;
    if (*begin == '+')
        ++begin;
    const char* end = cursor;
    if (*end == '+' || *end == '-')
        ++end;
    if (OverrideDigit(*end) < 0)
        return false;
    while (OverrideDigit(*end) >= 0)
        ++end;
    int parsed = 0;
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{})
        return false;
    cursor = result.ptr;
    value = parsed;
    return true;
}

inline bool ParseOverrideNumber(const char*& cursor, float& value) {
    const bool negative = *cursor == '-';
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    // Input is NUL-terminated: when cursor[0] is '0', cursor[1] is in bounds.
    const bool hex = cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X');
    if (hex)
        cursor += 2; // from_chars(hex) expects the significand without 0x.
    const char* begin = cursor;
    const char* firstNonzero = nullptr;
    const char* point = nullptr;
    bool digits = false;
    while (true) {
        const int digit = OverrideDigit(*cursor, hex);
        if (digit >= 0) {
            digits = true;
            if (digit != 0 && !firstNonzero)
                firstNonzero = cursor;
        } else if (*cursor == '.' && !point) {
            point = cursor;
        } else {
            break;
        }
        ++cursor;
    }
    if (!digits)
        return false; // Includes NaN/Inf (complete or partial): never assign them.

    const char* mantissaEnd = cursor;
    const char* exponent = cursor;
    bool exponentNegative = false;
    if (*cursor == (hex ? 'p' : 'e') || *cursor == (hex ? 'P' : 'E')) {
        ++cursor;
        exponentNegative = *cursor == '-';
        if (*cursor == '+' || *cursor == '-')
            ++cursor;
        exponent = cursor;
        while (OverrideDigit(*cursor) >= 0)
            ++cursor;
    }
    // scanf consumes an incomplete exponent ("1e", "1e+") but glibc
    // converts the mantissa. Keep the consumed end separate from conversion.
    const char* numberEnd = exponent == cursor ? mantissaEnd : cursor;
    float parsed = 0.0f;
    if (firstNonzero) {
        const auto result = std::from_chars(
            begin, numberEnd, parsed, hex ? std::chars_format::hex : std::chars_format::general);
        if (result.ec == std::errc::result_out_of_range) {
            // scanf accepts underflow as signed zero. from_chars reports both
            // underflow and overflow as out_of_range; distinguish by magnitude.
            // Saturate exponent accumulation beyond any possible mantissa shift.
            const long double bound = 4.0L * static_cast<long double>(mantissaEnd - begin) + 256.0L;
            long double power = 0.0L;
            for (const char* p = exponent; p != cursor; ++p) {
                power = power * 10.0L + static_cast<long double>(*p - '0');
                if (power > bound)
                    power = bound;
            }
            const char* decimalPoint = point ? point : mantissaEnd;
            long double order = static_cast<long double>(decimalPoint - firstNonzero);
            if (order > 0.0L)
                order -= 1.0L;
            order *= hex ? 4.0L : 1.0L;
            order += exponentNegative ? -power : power;
            if (order >= 0.0L)
                return false; // Overflow would feed infinity to the renderer.
            parsed = 0.0f;
        } else if (result.ec != std::errc{} || result.ptr != numberEnd) {
            return false;
        }
    }
    if (!std::isfinite(parsed))
        return false;
    value = negative ? -parsed : parsed;
    return true;
}

} // namespace Detail

// Apply the successfully converted prefix in order, leaving remaining defaults.
// Use C-locale scanf token/whitespace rules regardless of the process locale:
// %f/%d skip leading whitespace; literal commas must follow immediately.
// Hex floats remain supported. from_chars avoids MSVC unsafe-CRT diagnostics.
template<typename... Values>
inline std::size_t ParseCommaSeparatedPrefix(const std::string& text, Values&... values) {
    const char* cursor = text.c_str();
    std::size_t converted = 0;
    bool next = true;
    const auto parse = [&](auto& value) {
        if (!next)
            return;
        if (converted != 0) {
            if (*cursor != ',') {
                next = false;
                return;
            }
            ++cursor;
        }
        while (Detail::OverrideSpace(*cursor))
            ++cursor;
        using Value = std::decay_t<decltype(value)>;
        static_assert(std::is_same_v<Value, int> || std::is_same_v<Value, float>);
        if (!Detail::ParseOverrideNumber(cursor, value)) {
            next = false;
            return;
        }
        ++converted;
    };
    (parse(values), ...);
    return converted;
}

struct CaveAmbientOcclusion {
    float enabled = 1.0f;
    float maxDist = 24.0f;
    float floor = 0.06f;
    float thickness = 1.5f;
    int steps = 8;
};

inline CaveAmbientOcclusion
ResolveCaveAmbientOcclusion(const std::optional<std::string>& environment) {
    CaveAmbientOcclusion result;
    if (!environment)
        return result;

    // "enabled,maxDist,floor,steps,thickness"
    ParseCommaSeparatedPrefix(
        *environment, result.enabled, result.maxDist, result.floor, result.steps, result.thickness);
    return result;
}

} // namespace Luminumbra::Rendering
