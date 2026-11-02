#pragma once

#include "CaptureManifest.h"

namespace luminumbra::core {
struct ResolvedGraphics;
}
namespace Luminumbra::world {
class GameSession;
}
namespace Luminumbra::Rendering {
class RenderPipeline;

// Capture-only, owner-thread observation. Does not fence jobs, scan world state,
// read source files or advance simulation/render state. Call before readback.
CaptureManifestInput ObserveCaptureManifest(const world::GameSession& session,
                                            const RenderPipeline& pipeline,
                                            int width,
                                            int height,
                                            const luminumbra::core::ResolvedGraphics& graphics,
                                            bool wireframe,
                                            bool ui_overlay);
} // namespace Luminumbra::Rendering
