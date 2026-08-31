#pragma once

#include "ShaderReflection.h"

#include <string>

//   /   + /F.3: turn a Slang `-reflection-json`
// document into the SAME ReflectedLayout that GL program introspection produces
// (ShaderReflection.h:21-23), so a slangc-compiled HLSL module's reflected interface
// is directly comparable to the GL-introspected GLSL and the declared ExpectedLayout.
//
// Slang (the single-tool successor to the dxc + spirv-cross chain: it BOTH compiles
// HLSL -> SPIR-V/DXIL/GLSL AND reflects) emits a flat `parameters[]` array. Each
// parameter carries a `type.kind` ("resource" for textures/buffers, "samplerState"
// for standalone samplers, "constantBuffer" for cbuffers) and, for resources, a
// `type.baseShape` ("texture2D", "textureCube",...). The GL-comparable sampler set
// is the RESOURCE parameters whose shape is a sampled texture (name +
// dimensionality-derived GL type); standalone samplerState params are the
// split-binding-model artifact and are not part of the comparison.

namespace Luminumbra::Rendering {

// Map a Slang/SPIR-V resource shape ("texture2D", "texture2DArray", "textureCube",
// "texture3D",...) to the GL sampler enum GL introspection reports. 0 if not a
// sampled-texture shape (e.g. a structured/RW buffer), which the caller filters out.
GLenum ReflectedImageShapeToGlSampler(const std::string& shape);

// Parse a Slang `-reflection-json` document into a ReflectedLayout. Malformed JSON
// yields an empty layout (callers assert non-vacuity). samplers <- resource params
// with a sampled-texture shape; uniform_blocks <- constantBuffer params.
ReflectedLayout ReflectSlangReflectionJson(const std::string& json_text);

} // namespace Luminumbra::Rendering
