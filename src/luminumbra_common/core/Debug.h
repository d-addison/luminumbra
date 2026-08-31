#pragma once

#include "Log.h"

#include <cstdlib>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace Luminumbra::Debug {

template <typename... Args>
inline void LogCritical(Args&&... args) {
    auto& logger = ::Log::GetCoreLogger();
    if (logger) {
        logger->critical(std::forward<Args>(args)...);
    } else {
        spdlog::critical(std::forward<Args>(args)...);
    }
}

inline void Break() {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__clang__) || defined(__GNUC__)
    __builtin_trap();
#endif
}

template <typename... Args>
[[noreturn]] inline void AssertFailed(const char* expression, const char* file, int line, Args&&... args) {
    LogCritical("Assertion failed: {0} ({1}:{2})", expression, file, line);
    if constexpr (sizeof...(Args) > 0) {
        LogCritical(std::forward<Args>(args)...);
    }
    Break();
    std::abort();
}

} // namespace Luminumbra::Debug

#define LUMINUMBRA_ASSERT(condition, ...) \
    do { \
        if (!(condition)) { \
            ::Luminumbra::Debug::AssertFailed(#condition, __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (false)
