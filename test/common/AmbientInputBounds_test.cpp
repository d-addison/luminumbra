// Guard-page regression for FastNoise batch inputs.
//
// FastNoise's GenPositionArray2D tail loads a full SIMD vector from the x/y input arrays, so an
// input whose length is not a whole vector can read past its end. The probe places each input so
// it ends exactly at a page boundary, with the following page unmapped. The vendored tail load is
// expected to fault for an exact-size 2-element input, and a 16-element padded input must not.
// Linux only. The fault test is not applicable under AddressSanitizer, which intercepts the
// out-of-bounds read itself. Non-applicable cases pass and record a probe_status property, because
// CI rejects skipped tests.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

#include "luminumbra_common/systems/AmbientNoiseDispatch.h"

#if defined(__linux__)
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kUnderAddressSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kUnderAddressSanitizer = true;
#else
constexpr bool kUnderAddressSanitizer = false;
#endif
#else
constexpr bool kUnderAddressSanitizer = false;
#endif

#if defined(__linux__)

sigjmp_buf g_jump;
volatile sig_atomic_t g_armed = 0;

void OnSegv(int) {
    if (g_armed) {
        siglongjmp(g_jump, 1);
    }
    _exit(139);
}

FastSIMD::eLevel ActiveSimdLevel() {
    return Luminumbra::Systems::NewAmbientNoise<FastNoise::FractalFBm>()->GetSIMDLevel();
}

// True when the active ISA uses a wide vector tail load; a scalar level has no such load.
bool VectorTailLoadsBeyondCount() {
    const FastSIMD::eLevel level = ActiveSimdLevel();
    return level != FastSIMD::Level_Scalar && level != FastSIMD::Level_Null;
}

// Places each input so it ends at a page boundary with the next page unmapped, then runs the
// fractal batch call. Returns true when the call faulted, and false when the probe itself could
// not be set up (the failure is reported with ADD_FAILURE).
bool ProbeFaults(std::size_t buffer_floats, int count) {
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const std::size_t bytes = 2 * page;
    void* mem[2] = {MAP_FAILED, MAP_FAILED};
    const auto release = [&]() {
        for (void* region : mem) {
            if (region != MAP_FAILED) {
                munmap(region, bytes);
            }
        }
    };
    for (void*& region : mem) {
        region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) {
            ADD_FAILURE() << "mmap failed for the guard-page probe: " << std::strerror(errno);
            release();
            return false;
        }
    }

    float* inputs[2] = {nullptr, nullptr};
    for (int k = 0; k < 2; ++k) {
        char* base = static_cast<char*>(mem[k]);
        if (mprotect(base + page, page, PROT_NONE) != 0) {
            ADD_FAILURE() << "mprotect failed for the guard page: " << std::strerror(errno);
            release();
            return false;
        }
        float* in = reinterpret_cast<float*>(base + page) - buffer_floats;
        for (std::size_t i = 0; i < buffer_floats; ++i) {
            in[i] = i < static_cast<std::size_t>(count) ? 0.37f * static_cast<float>(i + 1) : 0.0f;
        }
        inputs[k] = in;
    }

    auto fractal = Luminumbra::Systems::NewAmbientNoise<FastNoise::FractalFBm>();
    auto simplex = Luminumbra::Systems::NewAmbientNoise<FastNoise::Simplex>();
    fractal->SetSource(simplex);
    fractal->SetOctaveCount(2);

    std::array<float, 16> out{};
    struct sigaction action {};
    struct sigaction previous {};
    action.sa_handler = OnSegv;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    if (sigaction(SIGSEGV, &action, &previous) != 0) {
        ADD_FAILURE() << "sigaction install failed for the guard-page probe: "
                      << std::strerror(errno);
        release();
        return false;
    }

    volatile bool faulted = false;
    if (sigsetjmp(g_jump, 1) == 0) {
        g_armed = 1;
        fractal->GenPositionArray2D(out.data(), count, inputs[0], inputs[1], 0.0f, 0.0f, 424242);
        g_armed = 0;
        faulted = false;
    } else {
        g_armed = 0;
        faulted = true;
    }

    if (sigaction(SIGSEGV, &previous, nullptr) != 0) {
        ADD_FAILURE() << "sigaction restore failed after the guard-page probe: "
                      << std::strerror(errno);
    }
    release();
    return faulted;
}

#endif // defined(__linux__)

// Empty when the fault probe applies to this build and ISA; otherwise the reason it does not.
std::string ProbeNotApplicableReason() {
    if (kUnderAddressSanitizer) {
        return "guard-page fault probe is not run under AddressSanitizer";
    }
#if defined(__linux__)
    if (!VectorTailLoadsBeyondCount()) {
        return "active SIMD level has no wide tail load";
    }
    return "";
#else
    return "guard-page probe is Linux only";
#endif
}

TEST(AmbientInputBounds, UnpaddedTwoElementInputFaultsAtActiveIsa) {
    const std::string reason = ProbeNotApplicableReason();
    if (!reason.empty()) {
        RecordProperty("probe_status", "not_applicable: " + reason);
        return;
    }
#if defined(__linux__)
    RecordProperty("noise_simd_level", static_cast<int>(ActiveSimdLevel()));
    RecordProperty("probe_status", "ran");
    EXPECT_TRUE(ProbeFaults(2, 2))
        << "the vendored tail load is expected to read past an exact-size input";
#endif
}

TEST(AmbientInputBounds, SixteenElementPaddedInputDoesNotFault) {
    // Runs on every Linux build, including AddressSanitizer and scalar levels: a padded input
    // never faults, so there is no reason to exclude it.
#if defined(__linux__)
    RecordProperty("noise_simd_level", static_cast<int>(ActiveSimdLevel()));
    RecordProperty("probe_status", "ran");
    EXPECT_FALSE(ProbeFaults(16, 2))
        << "a whole-vector padded input must not fault on the tail load";
#else
    RecordProperty("probe_status", "not_applicable: guard-page probe is Linux only");
#endif
}

} // namespace
