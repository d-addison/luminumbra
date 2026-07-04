#include "ShaderReflectionSpirv.h"

#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {

GLenum SpirvImageTypeToGlSampler(const std::string& t) {
    // Separate-image forms (HLSL Texture*) and combined forms (GLSL sampler*) both
    // map to the GL combined-sampler enum GL introspection reports.
    if (t == "texture2D" || t == "sampler2D") return GL_SAMPLER_2D;
    if (t == "texture2DArray" || t == "sampler2DArray") return GL_SAMPLER_2D_ARRAY;
    if (t == "textureCube" || t == "samplerCube") return GL_SAMPLER_CUBE;
    if (t == "texture3D" || t == "sampler3D") return GL_SAMPLER_3D;
    if (t == "texture2DShadow" || t == "sampler2DShadow") return GL_SAMPLER_2D_SHADOW;
    if (t == "textureCubeShadow" || t == "samplerCubeShadow") return GL_SAMPLER_CUBE_SHADOW;
    return 0;
}

ReflectedLayout ReflectSpirvReflectionJson(const std::string& json_text) {
    ReflectedLayout layout;
    const nlohmann::json doc = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return layout;
    }

    // HLSL Texture2D -> "separate_images"; GLSL combined sampler2D -> "textures".
    const auto add_images = [&](const char* key) {
        if (!doc.contains(key)) return;
        for (const auto& img : doc[key]) {
            ReflectedSampler s;
            s.name = img.value("name", std::string{});
            s.type = SpirvImageTypeToGlSampler(img.value("type", std::string{}));
            s.unit = img.value("binding", 0);
            layout.samplers.push_back(s);
        }
    };
    add_images("separate_images");
    add_images("textures");

    if (doc.contains("outputs")) {
        for (const auto& o : doc["outputs"]) {
            ReflectedOutput out;
            out.name = o.value("name", std::string{});
            out.location = o.value("location", -1);
            layout.outputs.push_back(out);
        }
    }
    const auto add_blocks = [&](const char* key, std::vector<ReflectedBlock>& into) {
        if (!doc.contains(key)) return;
        for (const auto& b : doc[key]) {
            ReflectedBlock block;
            block.name = b.value("name", std::string{});
            block.binding = b.value("binding", 0);
            into.push_back(block);
        }
    };
    add_blocks("ubos", layout.uniform_blocks);
    add_blocks("ssbos", layout.storage_blocks);

    return layout;
}

}  // namespace Luminumbra::Rendering
