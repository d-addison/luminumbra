#pragma once

//   /   "one abstraction, two layers": the RHI
// backend resource vocabulary that sits BENEATH the  render-resource handles.
//
// Layer 1 is RenderResourceHandle -- what a pass holds. Layer 2 (here) is
// the backend resource an owned registry entry is ACTUALLY made of: a GL name
// today, a Diligent IBuffer*/ITexture*/IPipelineState* tomorrow. A pass only ever
// sees the layer-1 handle, so it can never tell a GL name from a Diligent
// resource -- that is the whole point of the abstraction.
//
// These types are deliberately OPAQUE and Diligent-free: an RhiTexture is a plain
// 64-bit id (0 == null / not-yet-backed), and the concrete backend object it maps
// to lives entirely inside rhi/*.cpp (the sole Diligent include site). Nothing in
// this header names a Diligent type, so no Diligent header escapes rhi/ -- the
// invariant the RhiNoReexport frontier gate enforces mechanically.
//
// defines the vocabulary and gives owned registry entries an (unused) optional
// backing slot; the ids are populated only when a pass is first driven through a
// Diligent device. Until then every backing is null and the GL path is
// unchanged -- byte-identical by construction.

#include <cstdint>

namespace Luminumbra::Rendering::Rhi {

// The kind of backend resource an id refers to. Lets a single side table in rhi/
// stay type-checked without leaking the concrete Diligent object outward.
enum class RhiResourceKind : std::uint8_t {
    None,
    Buffer,
    Texture,
    Pipeline,
    Cmd,
};

// Opaque backend-resource handles. id == 0 means "no backend resource yet"
// (the resource is still purely its layer-1 GL object).
struct RhiBuffer {
    std::uint64_t id = 0;
};
struct RhiTexture {
    std::uint64_t id = 0;
};
struct RhiPipeline {
    std::uint64_t id = 0;
};
struct RhiCmd {
    std::uint64_t id = 0;
};

inline bool is_backed(RhiBuffer h) {
    return h.id != 0;
}
inline bool is_backed(RhiTexture h) {
    return h.id != 0;
}
inline bool is_backed(RhiPipeline h) {
    return h.id != 0;
}
inline bool is_backed(RhiCmd h) {
    return h.id != 0;
}

} // namespace Luminumbra::Rendering::Rhi
