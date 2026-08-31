#pragma once

//  (Batch-0 struct extraction). Hoisted OUT of the nested
// RenderPipeline::StaticModelTex so it can be named at namespace scope by the
// per-pass input structs (RenderInputs.h's GBufferPassInput holds a
// std::function<const StaticModelTex*(const std::string&)> lookup, which cannot
// reference a private nested type from outside the class).
//
//  static-model UV texture lane: per-meshPath albedo+normal array layers (and
// alpha-test flag) sampled by a static mesh's OWN UVs instead of the
// world-projected terrain triplanar. Consumed by GBufferPass static-mesh draws.
//
// RenderPipeline.h keeps `using StaticModelTex = Luminumbra::Rendering::StaticModelTex;`
// so the qualified name `RenderPipeline::StaticModelTex` (ImpostorBake.cpp:100)
// still resolves unchanged.

namespace Luminumbra::Rendering {

struct StaticModelTex {
    int albedoLayer = -1;
    int normalLayer = -1;
    bool alphaTest = false;
};

} // namespace Luminumbra::Rendering
