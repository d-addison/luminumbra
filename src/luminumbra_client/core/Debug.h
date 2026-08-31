#pragma once

#ifdef LUMINUMBRA_DEBUG
    #if defined(_MSC_VER)
#define LUMINUMBRA_DEBUGBREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define LUMINUMBRA_DEBUGBREAK() __builtin_trap()
#else
#error "Platform doesn't support debugbreak yet!"
#endif
#define LUMINUMBRA_ENABLE_ASSERTS
#else
#define LUMINUMBRA_DEBUGBREAK()
#endif

#ifdef LUMINUMBRA_ENABLE_ASSERTS
    #include "luminumbra_common/core/Log.h"
#define LUMINUMBRA_ASSERT(x, ...)                                                                  \
    {                                                                                              \
        if (!(x)) {                                                                                \
            LUMINUMBRA_CORE_ERROR("Assertion Failed: {0}", __VA_ARGS__);                           \
            LUMINUMBRA_DEBUGBREAK();                                                               \
        }                                                                                          \
    }
#else
    #define LUMINUMBRA_ASSERT(x, ...)
#endif
