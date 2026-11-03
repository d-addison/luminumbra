#pragma once

#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace Luminumbra::Startup {

struct CpuRegisters {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
};

struct X86Requirements {
    std::uint32_t leaf1_ecx;
    std::uint32_t leaf1_edx;
    std::uint32_t leaf7_ebx;
    std::uint64_t xcr0;
};

// FastNoise's AVX2 dispatch ladder requires SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT,
// XSAVE/OSXSAVE/AVX; its AVX2 TU adds FMA. Jolt's PUBLIC AVX2 options add
// BMI1/TZCNT, LZCNT and F16C even when individual Jolt options are disabled.
// MSVC /arch:AVX2 also permits BMI2 and MOVBE. Jolt's optional AVX512 profile
// adds F/DQ/VL (and MSVC permits CD/BW). Noise stays capped at AVX2.
// The EDX baseline is guaranteed on x64 and checked by FastNoise on x86.
constexpr X86Requirements CommonX86Requirements(bool msvc, bool avx512) noexcept {
    return {
        .leaf1_ecx = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 23) |
                     (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (msvc ? (1u << 22) : 0u),
        .leaf1_edx = (1u << 0) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26),
        .leaf7_ebx = (1u << 3) | (1u << 5) | (msvc ? (1u << 8) : 0u) |
                     (avx512 ? ((1u << 16) | (1u << 17) | (1u << 31) |
                                (msvc ? ((1u << 28) | (1u << 30)) : 0u))
                             : 0u),
        .xcr0 = avx512 ? 0xe6u : 0x6u,
    };
}

inline constexpr auto kCommonX86Requirements = CommonX86Requirements(
#if defined(_MSC_VER)
    true,
#else
    false,
#endif
#if defined(JPH_USE_AVX512)
    true
#else
    false
#endif
);

// The probe supplies CPUID leaves and XCR0 without entering the engine. Keeping
// the policy independent of the native probe permits checking virtual CPU
// profiles, including unavailable leaves and OS state, without executing them.
template<typename Probe>
constexpr bool
SupportsX86CommonLibrary(Probe& probe, X86Requirements required = kCommonX86Requirements) noexcept {
    CpuRegisters registers;
    if (!probe.Cpuid(0, registers) || registers.eax < 7 || !probe.Cpuid(1, registers) ||
        (registers.ecx & required.leaf1_ecx) != required.leaf1_ecx ||
        (registers.edx & required.leaf1_edx) != required.leaf1_edx) {
        return false;
    }
    if (!probe.Cpuid(7, registers) || (registers.ebx & required.leaf7_ebx) != required.leaf7_ebx) {
        return false;
    }
    // LZCNT lives in the extended leaf, independently of AVX2 and BMI1.
    if (!probe.Cpuid(0x80000000u, registers) || registers.eax < 0x80000001u ||
        !probe.Cpuid(0x80000001u, registers) || (registers.ecx & (1u << 5)) == 0) {
        return false;
    }
    // Never execute XGETBV before checking XSAVE and OSXSAVE above.
    std::uint64_t xcr0 = 0;
    return probe.ReadXcr0(xcr0) && (xcr0 & required.xcr0) == required.xcr0;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
struct NativeCpuProbe {
    bool Cpuid(std::uint32_t leaf, CpuRegisters& registers) const noexcept {
#if defined(_MSC_VER)
        int values[4]{};
        __cpuidex(values, static_cast<int>(leaf), 0);
        registers = {.eax = static_cast<std::uint32_t>(values[0]),
                     .ebx = static_cast<std::uint32_t>(values[1]),
                     .ecx = static_cast<std::uint32_t>(values[2]),
                     .edx = static_cast<std::uint32_t>(values[3])};
#else
        __cpuid_count(leaf, 0, registers.eax, registers.ebx, registers.ecx, registers.edx);
#endif
        return true;
    }

    bool ReadXcr0(std::uint64_t& value) const noexcept {
#if defined(_MSC_VER)
        value = _xgetbv(0);
#else
        CpuRegisters registers;
        ReadXcr0Words(registers);
        value = (static_cast<std::uint64_t>(registers.edx) << 32) | registers.eax;
#endif
        return true;
    }

private:
#if !defined(_MSC_VER)
    static void ReadXcr0Words(CpuRegisters& registers) noexcept {
        __asm__ volatile("xgetbv" : "=a"(registers.eax), "=d"(registers.edx) : "c"(0));
    }
#endif
};
#endif

inline bool SupportsCommonLibrary() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    NativeCpuProbe probe;
    return SupportsX86CommonLibrary(probe);
#else
    // Non-x86 applications retain their native dependency dispatch. Discovery
    // separately refuses non-x86 because its numeric profile is x86-specific.
    return true;
#endif
}

inline constexpr const char* kUnsupportedCommonIsa =
    "unsupported common-library ISA or OS vector state";

} // namespace Luminumbra::Startup
