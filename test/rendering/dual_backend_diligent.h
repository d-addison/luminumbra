#pragma once
// GPU-03 + GPU-P05 (spec 021 rank 66) leg B: render the calibration cube through
// Diligent's GL backend and hand the bytes back to the glad-based FLIP harness.
//
// This is the ONLY render TU that includes Diligent headers (the same discipline
// as rhi/Device.cpp on the client side): no Diligent type escapes here. The result
// carries plain std types + a diagnostic string so dual_backend_flip_test.cpp stays
// glad-only and Diligent-header-free.

#include "basic_cube_harness.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luminumbra_test {

struct DiligentRenderResult {
    bool available = false;             // a Diligent GL device rendered the pass
    std::string diagnostic;             // failure/context detail (empty on success)
    std::vector<std::uint8_t> pixels;   // RGBA8, kCubeWidth*kCubeHeight*4, TOP-DOWN
                                        // (Diligent origin); caller flips to compare
                                        // against glReadPixels' bottom-up golden.
};

// Render the cube into an RGBA8 render target via a Diligent GL device attached to
// the GL context CURRENT on the calling thread (the caller stands one up first,
// exactly like CreateHeadlessDevice(Backend::Gl)). No swapchain/window. On any
// failure `available` is false and `diagnostic` explains; `pixels` is then empty.
DiligentRenderResult RenderCubeDiligentGl(const std::vector<MeshVertex>& mesh,
                                          const RenderParams& params);

}  // namespace luminumbra_test
