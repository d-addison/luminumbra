#include "ShaderReflectionSpirv.h"

#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {

GLenum ReflectedImageShapeToGlSampler(const std::string& shape) {
    if (shape == "texture2D") return GL_SAMPLER_2D;
    if (shape == "texture2DArray") return GL_SAMPLER_2D_ARRAY;
    if (shape == "textureCube") return GL_SAMPLER_CUBE;
    if (shape == "textureCubeArray") return GL_SAMPLER_CUBE_MAP_ARRAY;
    if (shape == "texture3D") return GL_SAMPLER_3D;
    if (shape == "texture1D") return GL_SAMPLER_1D;
    return 0;
}

ReflectedLayout ReflectSlangReflectionJson(const std::string& json_text) {
    ReflectedLayout layout;
    const nlohmann::json doc = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("parameters")) {
        return layout;
    }

    for (const auto& param : doc["parameters"]) {
        if (!param.contains("type") || !param["type"].is_object()) {
            continue;
        }
        const auto& type = param["type"];
        const std::string kind = type.value("kind", std::string{});

        if (kind == "resource") {
            // Only sampled textures map to the GL combined-sampler set; structured/RW
            // buffers (shape != texture*) return 0 and are skipped.
            const GLenum gl_type = ReflectedImageShapeToGlSampler(type.value("baseShape", std::string{}));
            if (gl_type != 0) {
                ReflectedSampler s;
                s.name = param.value("name", std::string{});
                s.type = gl_type;
                if (param.contains("binding") && param["binding"].is_object()) {
                    s.unit = param["binding"].value("index", 0);
                }
                layout.samplers.push_back(s);
            }
        } else if (kind == "constantBuffer") {
            ReflectedBlock block;
            block.name = param.value("name", std::string{});
            if (param.contains("binding") && param["binding"].is_object()) {
                block.binding = param["binding"].value("index", 0);
            }
            layout.uniform_blocks.push_back(block);
        }
        // kind == "samplerState": ignored -- the split-binding-model artifact, not
        // part of the GL-comparable interface (GL sees combined samplers).
    }

    return layout;
}

}  // namespace Luminumbra::Rendering
