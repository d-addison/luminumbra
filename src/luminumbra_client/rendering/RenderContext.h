#pragma once

#include "RenderResourceHandles.h"
#include "RenderResourceRegistry.h"
#include "../../include/luminumbra/core/Types.h"

// Spec 016 (FR-A-001/002): the per-frame render PASS CONTRACT.
//
// A pass receives a RenderContext& (frame/camera/time + screen/target geometry +
// typed resource handles via the registry) instead of `RenderPipeline&`. This is
// the engine-owned seam: passes read what they declare from the context and write
// to handles, with no access to RenderPipeline privates and no `friend`.
//
// It is also the seam spec 014's RHI backend implements BEHIND: the handles here
// are backend-agnostic; today they wrap GL names, later a Diligent backend.
//
// Minimal during the FinalBlit pilot (016-P1-T01) and grown additively as each
// pass converts; fields are added, never removed, so a converting pass only ever
// gains context it can read.

namespace Luminumbra::Rendering {

class Camera;

struct RenderContext {
    // Frame / camera / time.
    u64 frame_index = 0;
    const Camera* camera = nullptr;
    float delta_time = 0.0f;

    // Backbuffer / screen geometry.
    u32 screen_width = 0;
    u32 screen_height = 0;

    // Offscreen-target state (Spec 002 Item 1 preview path). When active the
    // final image is written to offscreen_fbo at offscreen_w/h instead of the
    // default framebuffer.
    bool offscreen_active = false;
    u32 offscreen_fbo = 0;
    u32 offscreen_w = 0;
    u32 offscreen_h = 0;

    // Typed resource access.
    RenderResourceRegistry* registry = nullptr;

    // The lit-scene color source produced upstream (adopted into the registry as
    // "lit_scene"). The FinalBlit pass reads this and resolves it to the target.
    FboHandle lit_scene{};

    // Destination resolution helpers (screen vs offscreen preview target).
    FboHandle dest_fbo() const {
        return offscreen_active ? adopt_fbo(offscreen_fbo) : default_framebuffer();
    }
    u32 dest_width() const { return offscreen_active ? offscreen_w : screen_width; }
    u32 dest_height() const { return offscreen_active ? offscreen_h : screen_height; }
};

} // namespace Luminumbra::Rendering
