#include "startup/CommonLibraryIsa.h"

#include <array>
#include <cstdint>

namespace {
using Luminumbra::Startup::CommonX86Requirements;
using Luminumbra::Startup::CpuRegisters;
using Luminumbra::Startup::SupportsX86CommonLibrary;

// A virtual CPU profile. These assertions exercise the shipping policy without
// executing any instructions from the represented (possibly unsupported) ISA.
struct ProfileData {
    std::uint32_t basic_max = 7;
    std::uint32_t extended_max = 0x80000001u;
    std::uint32_t leaf1_ecx = 0xffffffffu;
    std::uint32_t leaf1_edx = 0xffffffffu;
    std::uint32_t leaf7_ebx = 0xffffffffu;
    std::uint32_t extended_ecx = 0xffffffffu;
    std::uint64_t xcr0 = 0xe6u;
    std::uint32_t unreadable_leaf = 0xffffffffu;
    bool readable_xcr0 = true;
    bool read_xcr0 = false;
};

struct ProfileProbe : ProfileData {
    constexpr bool Cpuid(std::uint32_t leaf, CpuRegisters& registers) const noexcept {
        if (leaf == unreadable_leaf) {
            return false;
        }
        registers = {};
        switch (leaf) {
            case 0:
                registers.eax = basic_max;
                break;
            case 1:
                registers.ecx = leaf1_ecx;
                registers.edx = leaf1_edx;
                break;
            case 7:
                registers.ebx = leaf7_ebx;
                break;
            case 0x80000000u:
                registers.eax = extended_max;
                break;
            case 0x80000001u:
                registers.ecx = extended_ecx;
                break;
            default:
                return false;
        }
        return true;
    }

    constexpr bool ReadXcr0(std::uint64_t& value) noexcept {
        read_xcr0 = true;
        value = xcr0;
        return readable_xcr0;
    }
};

constexpr bool RefusesMissingFeatures(bool msvc, bool avx512) {
    const auto requirements = CommonX86Requirements(msvc, avx512);
    ProfileProbe supported;
    if (!SupportsX86CommonLibrary(supported, requirements) || !supported.read_xcr0) {
        return false;
    }
    // Independent bit oracle: in particular, AVX2 with FMA masked must refuse.
    for (const auto bit : std::array{0u, 9u, 12u, 19u, 20u, 23u, 26u, 27u, 28u, 29u}) {
        ProfileProbe masked;
        masked.leaf1_ecx &= ~(1u << bit);
        if (SupportsX86CommonLibrary(masked, requirements) || masked.read_xcr0) {
            return false;
        }
    }
    for (const auto bit : std::array{0u, 15u, 23u, 24u, 25u, 26u}) {
        ProfileProbe masked;
        masked.leaf1_edx &= ~(1u << bit);
        if (SupportsX86CommonLibrary(masked, requirements) || masked.read_xcr0) {
            return false;
        }
    }
    for (const auto bit : std::array{3u, 5u}) {
        ProfileProbe masked;
        masked.leaf7_ebx &= ~(1u << bit);
        if (SupportsX86CommonLibrary(masked, requirements)) {
            return false;
        }
    }
    ProfileProbe no_lzcnt;
    no_lzcnt.extended_ecx &= ~(1u << 5);
    if (SupportsX86CommonLibrary(no_lzcnt, requirements)) {
        return false;
    }
    // Disabled OS AVX state: neither, only XMM, and only YMM all refuse.
    for (const auto state : std::array{0u, 2u, 4u}) {
        ProfileProbe masked;
        masked.xcr0 = state;
        if (SupportsX86CommonLibrary(masked, requirements) || !masked.read_xcr0) {
            return false;
        }
    }
    for (const auto leaf : std::array{0u, 1u, 7u, 0x80000000u, 0x80000001u}) {
        ProfileProbe unreadable;
        unreadable.unreadable_leaf = leaf;
        if (SupportsX86CommonLibrary(unreadable, requirements) || unreadable.read_xcr0) {
            return false;
        }
    }
    ProfileProbe no_os_state;
    no_os_state.readable_xcr0 = false;
    if (SupportsX86CommonLibrary(no_os_state, requirements)) {
        return false;
    }
    for (const auto maximum : std::array{0u, 1u, 6u}) {
        ProfileProbe partial;
        partial.basic_max = maximum;
        if (SupportsX86CommonLibrary(partial, requirements) || partial.read_xcr0) {
            return false;
        }
    }
    for (const auto maximum : std::array{0u, 0x80000000u}) {
        ProfileProbe partial;
        partial.extended_max = maximum;
        if (SupportsX86CommonLibrary(partial, requirements) || partial.read_xcr0) {
            return false;
        }
    }
    ProfileProbe movbe;
    movbe.leaf1_ecx &= ~(1u << 22);
    ProfileProbe bmi2;
    bmi2.leaf7_ebx &= ~(1u << 8);
    if (SupportsX86CommonLibrary(movbe, requirements) == msvc ||
        SupportsX86CommonLibrary(bmi2, requirements) == msvc) {
        return false;
    }
    for (const auto bit : std::array{16u, 17u, 31u}) {
        ProfileProbe masked;
        masked.leaf7_ebx &= ~(1u << bit);
        if (SupportsX86CommonLibrary(masked, requirements) == avx512) {
            return false;
        }
    }
    for (const auto bit : std::array{28u, 30u}) {
        ProfileProbe masked;
        masked.leaf7_ebx &= ~(1u << bit);
        if (SupportsX86CommonLibrary(masked, requirements) == (msvc && avx512)) {
            return false;
        }
    }
    for (const auto bit : std::array{5u, 6u, 7u}) {
        ProfileProbe masked;
        masked.xcr0 &= ~(std::uint64_t{1} << bit);
        if (SupportsX86CommonLibrary(masked, requirements) == avx512) {
            return false;
        }
    }
    return true;
}

static_assert(RefusesMissingFeatures(false, false));
static_assert(RefusesMissingFeatures(true, false));
static_assert(RefusesMissingFeatures(false, true));
static_assert(RefusesMissingFeatures(true, true));
static_assert([] {
    ProfileProbe masked;
    masked.leaf1_ecx &= ~(1u << 12);
    return !SupportsX86CommonLibrary(masked);
}());
static_assert([] {
    ProfileProbe masked;
    masked.xcr0 = 0;
    return !SupportsX86CommonLibrary(masked);
}());
} // namespace
