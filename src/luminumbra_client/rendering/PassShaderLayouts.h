#pragma once

#include "ShaderReflection.h"

#include <string>
#include <string_view>
#include <vector>

//   ( + ): the single, ENUMERABLE source of truth for
// every render pass's expected shader-resource layout.
//
// Each pass shader that binds samplers declares its ExpectedLayout HERE (once), and
// both consumers read it from here:
//   * the pass itself, in init_shader, calls Shader::ValidateLayout with its entry
//     (the load-time "renders garbage" tripwire + the hot-reload rollback arm);
//   * shader_reflection_test loads each entry's shader against a hidden GL context,
//     reflects it, and asserts the ExpectedLayout validates clean AND non-vacuously
//     (no stripped/absent expected samplers), per pass.
//
// A registry rather than per-pass inline declarations because the layout set must be
// enumerable without linking every pass TU — the  fixture test and the
//  coverage gate both need the full list. It is also what 's SPIRV-Cross
// reflection path (ranks 64-66) validates the cross-compiled modules against.
//
// Entries declare ONLY samplers the shader actually SAMPLES: the GL linker strips
// declared-but-unused uniforms, and an absent expected sampler is a soft warning, so
// listing a stripped sampler would make the non-vacuity assertion fail. A sampler the
// pass binds but the shader never reads is intentionally omitted (see LightingPass's
// u_terrainTextures note).

namespace Luminumbra::Rendering {

struct PassShaderLayout {
    std::string layout_name; // unique per SHADER (a multi-shader pass has several)
    std::string vert;        // shader vertex path, relative to the runtime root
    std::string frag;        // shader fragment path, relative to the runtime root
    ExpectedLayout expected; // the samplers this shader binds (sampled-only)
};

// The full set, in a stable declaration order. Built once (function-local static).
const std::vector<PassShaderLayout>& AllPassShaderLayouts();

// Look up a full entry (paths + layout) by its layout_name (nullptr if absent). The
//  fixture test uses this to load each entry's real shader and validate it.
const PassShaderLayout* FindPassShaderLayout(std::string_view layout_name);

// Look up a single expected layout by its layout_name (nullptr if absent). Passes call
// this in init_shader so the ExpectedLayout lives in exactly one place.
const ExpectedLayout* FindPassExpectedLayout(std::string_view layout_name);

} // namespace Luminumbra::Rendering
