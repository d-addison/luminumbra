#include "ShaderReflection.h"

#include <array>
#include <cstdio>

namespace Luminumbra::Rendering {

namespace {

const ReflectedSampler* find_sampler_impl(const std::vector<ReflectedSampler>& v, std::string_view name) {
    for (const auto& s : v) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
const ReflectedBlock* find_block_impl(const std::vector<ReflectedBlock>& v, std::string_view name) {
    for (const auto& b : v) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

} // namespace

const ReflectedSampler* ReflectedLayout::find_sampler(std::string_view name) const {
    return find_sampler_impl(samplers, name);
}
const ReflectedBlock* ReflectedLayout::find_uniform_block(std::string_view name) const {
    return find_block_impl(uniform_blocks, name);
}
const ReflectedBlock* ReflectedLayout::find_storage_block(std::string_view name) const {
    return find_block_impl(storage_blocks, name);
}

bool IsSamplerType(GLenum type) {
    switch (type) {
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_INT_SAMPLER_1D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
            return true;
        default:
            return false;
    }
}

std::string GlTypeName(GLenum type) {
    switch (type) {
        case GL_SAMPLER_2D:                 return "sampler2D";
        case GL_SAMPLER_3D:                 return "sampler3D";
        case GL_SAMPLER_CUBE:               return "samplerCube";
        case GL_SAMPLER_2D_ARRAY:           return "sampler2DArray";
        case GL_SAMPLER_2D_SHADOW:          return "sampler2DShadow";
        case GL_SAMPLER_2D_ARRAY_SHADOW:    return "sampler2DArrayShadow";
        case GL_SAMPLER_CUBE_MAP_ARRAY:     return "samplerCubeArray";
        case GL_SAMPLER_1D:                 return "sampler1D";
        case GL_INT_SAMPLER_2D:             return "isampler2D";
        case GL_UNSIGNED_INT_SAMPLER_2D:    return "usampler2D";
        case GL_INT_SAMPLER_2D_ARRAY:       return "isampler2DArray";
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: return "usampler2DArray";
        case GL_IMAGE_2D:                   return "image2D";
        case GL_IMAGE_2D_ARRAY:            return "image2DArray";
        case GL_FLOAT:                      return "float";
        case GL_FLOAT_VEC2:                 return "vec2";
        case GL_FLOAT_VEC3:                 return "vec3";
        case GL_FLOAT_VEC4:                 return "vec4";
        case GL_INT:                        return "int";
        case GL_FLOAT_MAT4:                 return "mat4";
        default: {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(type));
            return std::string(buf);
        }
    }
}

ReflectedLayout ReflectProgramLayout(GLuint program) {
    ReflectedLayout layout;
    if (program == 0) return layout;

    // --- Active uniforms: keep only the sampler-typed ones. The default-block
    // scalar/vector uniforms are validated by the uniform setters elsewhere; the
    // resource-binding contract is the samplers + blocks. ----------------------
    GLint num_uniforms = 0;
    glGetProgramInterfaceiv(program, GL_UNIFORM, GL_ACTIVE_RESOURCES, &num_uniforms);
    const std::array<GLenum, 3> uni_props = {GL_TYPE, GL_LOCATION, GL_NAME_LENGTH};
    for (GLint i = 0; i < num_uniforms; ++i) {
        std::array<GLint, 3> vals{};
        glGetProgramResourceiv(program, GL_UNIFORM, static_cast<GLuint>(i),
                               static_cast<GLsizei>(uni_props.size()), uni_props.data(),
                               static_cast<GLsizei>(vals.size()), nullptr, vals.data());
        const GLenum type = static_cast<GLenum>(vals[0]);
        if (!IsSamplerType(type)) continue;
        const GLint location = vals[1];
        const GLint name_len = vals[2];
        if (name_len <= 0) continue;
        std::string name(static_cast<size_t>(name_len), '\0');
        glGetProgramResourceName(program, GL_UNIFORM, static_cast<GLuint>(i),
                                 name_len, nullptr, name.data());
        if (!name.empty() && name.back() == '\0') name.pop_back();
        // Array samplers introspect as "name[0]"; normalise to the base name so a
        // pass declares the plain identifier it wrote in the shader.
        const auto bracket = name.find('[');
        if (bracket != std::string::npos) name.erase(bracket);

        ReflectedSampler s;
        s.name = name;
        s.type = type;
        s.location = location;
        s.unit = 0;
        if (location >= 0) {
            // Post-link the sampler holds its layout(binding=) default, else 0.
            glGetUniformiv(program, location, &s.unit);
        }
        layout.samplers.push_back(std::move(s));
    }

    // --- Uniform blocks (UBOs). ------------------------------------------------
    GLint num_ubo = 0;
    glGetProgramInterfaceiv(program, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &num_ubo);
    const std::array<GLenum, 2> blk_props = {GL_BUFFER_BINDING, GL_NAME_LENGTH};
    for (GLint i = 0; i < num_ubo; ++i) {
        std::array<GLint, 2> vals{};
        glGetProgramResourceiv(program, GL_UNIFORM_BLOCK, static_cast<GLuint>(i),
                               static_cast<GLsizei>(blk_props.size()), blk_props.data(),
                               static_cast<GLsizei>(vals.size()), nullptr, vals.data());
        const GLint binding = vals[0];
        const GLint name_len = vals[1];
        if (name_len <= 0) continue;
        std::string name(static_cast<size_t>(name_len), '\0');
        glGetProgramResourceName(program, GL_UNIFORM_BLOCK, static_cast<GLuint>(i),
                                 name_len, nullptr, name.data());
        if (!name.empty() && name.back() == '\0') name.pop_back();
        layout.uniform_blocks.push_back(ReflectedBlock{std::move(name), binding});
    }

    // --- Shader-storage blocks (SSBOs). ---------------------------------------
    GLint num_ssbo = 0;
    glGetProgramInterfaceiv(program, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &num_ssbo);
    for (GLint i = 0; i < num_ssbo; ++i) {
        std::array<GLint, 2> vals{};
        glGetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(i),
                               static_cast<GLsizei>(blk_props.size()), blk_props.data(),
                               static_cast<GLsizei>(vals.size()), nullptr, vals.data());
        const GLint binding = vals[0];
        const GLint name_len = vals[1];
        if (name_len <= 0) continue;
        std::string name(static_cast<size_t>(name_len), '\0');
        glGetProgramResourceName(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(i),
                                 name_len, nullptr, name.data());
        if (!name.empty() && name.back() == '\0') name.pop_back();
        layout.storage_blocks.push_back(ReflectedBlock{std::move(name), binding});
    }

    // --- Fragment outputs (draw-buffer / attachment slots). -------------------
    GLint num_out = 0;
    glGetProgramInterfaceiv(program, GL_PROGRAM_OUTPUT, GL_ACTIVE_RESOURCES, &num_out);
    const std::array<GLenum, 2> out_props = {GL_LOCATION, GL_NAME_LENGTH};
    for (GLint i = 0; i < num_out; ++i) {
        std::array<GLint, 2> vals{};
        glGetProgramResourceiv(program, GL_PROGRAM_OUTPUT, static_cast<GLuint>(i),
                               static_cast<GLsizei>(out_props.size()), out_props.data(),
                               static_cast<GLsizei>(vals.size()), nullptr, vals.data());
        const GLint location = vals[0];
        const GLint name_len = vals[1];
        if (name_len <= 0) continue;
        std::string name(static_cast<size_t>(name_len), '\0');
        glGetProgramResourceName(program, GL_PROGRAM_OUTPUT, static_cast<GLuint>(i),
                                 name_len, nullptr, name.data());
        if (!name.empty() && name.back() == '\0') name.pop_back();
        layout.outputs.push_back(ReflectedOutput{std::move(name), location});
    }

    return layout;
}

ValidationResult ValidateReflectedLayout(const ReflectedLayout& reflected,
                                         const ExpectedLayout& expected) {
    ValidationResult res;
    std::string errs;
    std::string warns;

    for (const auto& es : expected.samplers) {
        const ReflectedSampler* rs = reflected.find_sampler(es.name);
        if (!rs) {
            // The linker strips declared-but-unused uniforms -> not a hard fail.
            warns += "  sampler '" + es.name + "' (" + GlTypeName(es.type) +
                     ") not active in the linked program (stripped-unused?)\n";
            continue;
        }
        if (es.type != 0 && rs->type != es.type) {
            errs += "  sampler '" + es.name + "' TYPE mismatch: shader declares " +
                    GlTypeName(rs->type) + ", pass adopts it as " + GlTypeName(es.type) + "\n";
        }
        if (es.unit >= 0 && rs->unit != es.unit) {
            errs += "  sampler '" + es.name + "' UNIT mismatch: shader layout(binding)=" +
                    std::to_string(rs->unit) + ", pass binds unit " + std::to_string(es.unit) + "\n";
        }
    }

    auto check_block = [&](const std::vector<ExpectedBlock>& exp,
                           const std::vector<ReflectedBlock>& refl,
                           const char* kind) {
        for (const auto& eb : exp) {
            const ReflectedBlock* rb = find_block_impl(refl, eb.name);
            if (!rb) {
                warns += std::string("  ") + kind + " '" + eb.name +
                         "' not active in the linked program\n";
                continue;
            }
            if (eb.binding >= 0 && rb->binding != eb.binding) {
                errs += std::string("  ") + kind + " '" + eb.name +
                        "' BINDING mismatch: shader binding=" + std::to_string(rb->binding) +
                        ", pass expects " + std::to_string(eb.binding) + "\n";
            }
        }
    };
    check_block(expected.uniform_blocks, reflected.uniform_blocks, "UBO");
    check_block(expected.storage_blocks, reflected.storage_blocks, "SSBO");

    res.ok = errs.empty();
    res.had_warning = !warns.empty();
    if (!errs.empty()) {
        res.diagnostic = "reflected-layout MISMATCH for pass '" + expected.pass_name + "':\n" + errs;
        if (!warns.empty()) res.diagnostic += warns;
    } else if (!warns.empty()) {
        res.diagnostic = "reflected-layout warnings for pass '" + expected.pass_name + "':\n" + warns;
    }
    return res;
}

} // namespace Luminumbra::Rendering
