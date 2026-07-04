#pragma once

#include "ShaderReflection.h"

#include <string>

// spec 021 GPU-P04 / spec 014 FR-A.2: turn a spirv-cross `--reflect` JSON document
// into the SAME ReflectedLayout that GL program introspection produces
// (ShaderReflection.h:21-23), so a DXC-compiled HLSL module's reflected interface
// is directly comparable to the GL-introspected GLSL and the declared ExpectedLayout.
//
// HLSL's separate texture/sampler model: a `Texture2D` reflects under
// "separate_images" and a `SamplerState` under "separate_samplers". GL introspection
// sees a single combined `sampler2D`. The GL-comparable sampler set is therefore the
// IMAGES (name + dimensionality-derived GL type); the standalone SamplerState is a
// binding-model artifact and is not part of the comparison.

namespace Luminumbra::Rendering {

// Map a spirv-cross reflected image/sampler type string ("texture2D",
// "texture2DArray", "textureCube", "sampler2D", ...) to the GL sampler enum GL
// introspection reports for the equivalent combined sampler. Returns 0 if unknown.
GLenum SpirvImageTypeToGlSampler(const std::string& spirv_type);

// Parse a spirv-cross `--reflect` JSON document. Malformed JSON yields an empty
// layout (callers assert non-vacuity). samplers <- separate_images (+ textures if the
// module used combined samplers); outputs <- outputs; uniform/storage blocks <-
// ubos/ssbos.
ReflectedLayout ReflectSpirvReflectionJson(const std::string& json_text);

}  // namespace Luminumbra::Rendering
