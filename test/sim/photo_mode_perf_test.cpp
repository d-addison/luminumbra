// Photography capture-loop complexity smoke test.
//
// This pins the cost of the PER-SHUTTER scoring work — BuildShotInput + the full
// EvaluateShot/CaptureShot grade over a representative frame — NOT the render. The
// shutter must feel instantaneous; the scoring path is pure float math + small
// vectors, so a generous wall-clock budget guards against an accidental O(N^2) or
// allocation blowup creeping into the loop later.
//
// This is a generous gross-complexity ceiling, not comparable performance
// evidence. Release regressions are evaluated by paired base/head observations.
// Headless, no GL, no entt: it times only the pure capture flow.
//
// N = 2000 captures over a 6-subject frame. The pure scoring path runs
// in well under 1 us/capture in release and a few us in debug; the budget is set to
// 250 us/capture so even a heavily-loaded debug CI host clears it while a true
// regression (e.g. a per-capture heap thrash or quadratic blowup) does not.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "game/PhotoMode.h"

namespace {

using luminumbra::game::BuildShotInput;
using luminumbra::game::CaptureShot;
using luminumbra::game::LensSettings;
using luminumbra::game::PhotoCodex;
using luminumbra::game::PhotoSubjectView;
using luminumbra::game::ShotInput;
using luminumbra::game::ShotVerdict;

// A representative 6-subject frame (a few species at varied placement/depth).
std::vector<PhotoSubjectView> MakePerfFrame() {
    std::vector<PhotoSubjectView> views;
    for (int i = 0; i < 6; ++i) {
        PhotoSubjectView v;
        v.ndc_x = -0.5f + 0.2f * static_cast<float>(i);
        v.ndc_y = 0.4f - 0.15f * static_cast<float>(i);
        v.size = 0.5f - 0.05f * static_cast<float>(i);
        v.light = 0.6f;
        v.species_id = i % 3;
        v.distance_m = 3.0f + static_cast<float>(i);
        v.size_m = 0.5f;
        v.in_frustum = true;
        views.push_back(v);
    }
    return views;
}

LensSettings MakePerfLens() {
    LensSettings lens;
    lens.focal_length_mm = 85.0f;
    lens.aperture_f = 2.0f;
    lens.focus_distance_m = 4.0f;
    lens.iso = 200.0f;
    lens.shutter_s = 0.004f;
    return lens;
}

// ---------------------------------------------------------------------------
// Pinned per-capture budget. See the file header for the machine + N rationale.
// ---------------------------------------------------------------------------
constexpr int kPerfCaptures = 2000;
constexpr double kBudgetUsPerCapture = 250.0; // generous gross-complexity ceiling

TEST(PhotoModePerf, PerShutterWorkUnderBudget) {
    const std::vector<PhotoSubjectView> views = MakePerfFrame();
    const LensSettings lens = MakePerfLens();

    // Warm the path once (touch the libraries' first-use cost, if any).
    {
        PhotoCodex warm;
        const ShotInput in = BuildShotInput(views, lens, 0.5f);
        const ShotVerdict v = CaptureShot(warm, in);
        EXPECT_GE(v.total, 0.0f);
    }

    PhotoCodex codex;
    volatile float sink = 0.0f; // defeat dead-code elimination of the scoring result
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPerfCaptures; ++i) {
        const ShotInput in = BuildShotInput(views, lens, 0.5f);
        const ShotVerdict v = CaptureShot(codex, in);
        sink += v.total;
    }
    const auto t1 = std::chrono::steady_clock::now();
    (void)sink;

    const double total_us =
        std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(t1 - t0).count();
    const double us_per_capture = total_us / static_cast<double>(kPerfCaptures);

    EXPECT_LT(us_per_capture, kBudgetUsPerCapture)
        << "per-shutter scoring work " << us_per_capture << " us/capture exceeds the "
        << kBudgetUsPerCapture << " us budget over " << kPerfCaptures << " captures";

    // The loop actually ran the grade (codex was fed every capture).
    EXPECT_GT(codex.species_count(), 0u);
}

} // namespace
