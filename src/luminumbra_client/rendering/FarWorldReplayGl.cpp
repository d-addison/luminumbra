#include "FarWorldReplayGl.h"
#include <array>
#include <glad/glad.h>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Rendering {
namespace {
class GlAccess final : public ReplayStateAccess {
    static constexpr std::array<GLenum, 9> kBufferTargets = {GL_ARRAY_BUFFER,
                                                             GL_ELEMENT_ARRAY_BUFFER,
                                                             GL_DRAW_INDIRECT_BUFFER,
                                                             GL_DISPATCH_INDIRECT_BUFFER,
                                                             GL_SHADER_STORAGE_BUFFER,
                                                             GL_UNIFORM_BUFFER,
                                                             GL_COPY_READ_BUFFER,
                                                             GL_COPY_WRITE_BUFFER,
                                                             GL_PIXEL_PACK_BUFFER};
    static constexpr std::array<GLenum, 9> kBufferBindings = {GL_ARRAY_BUFFER_BINDING,
                                                              GL_ELEMENT_ARRAY_BUFFER_BINDING,
                                                              GL_DRAW_INDIRECT_BUFFER_BINDING,
                                                              GL_DISPATCH_INDIRECT_BUFFER_BINDING,
                                                              GL_SHADER_STORAGE_BUFFER_BINDING,
                                                              GL_UNIFORM_BUFFER_BINDING,
                                                              GL_COPY_READ_BUFFER_BINDING,
                                                              GL_COPY_WRITE_BUFFER_BINDING,
                                                              GL_PIXEL_PACK_BUFFER_BINDING};
    static constexpr std::array<GLenum, 9> kEnables = {GL_DEPTH_TEST,
                                                       GL_CULL_FACE,
                                                       GL_SCISSOR_TEST,
                                                       GL_POLYGON_OFFSET_FILL,
                                                       GL_POLYGON_OFFSET_LINE,
                                                       GL_RASTERIZER_DISCARD,
                                                       GL_FRAMEBUFFER_SRGB,
                                                       GL_CLIP_DISTANCE0,
                                                       GL_CLIP_DISTANCE1};
    struct Range {
        GLint name = 0;
        GLint64 start = 0, size = 0;
    };
    struct Texture {
        std::array<GLint, 5> names{};
        GLint sampler = 0;
    };
    struct Image {
        GLint name = 0, level = 0, layered = 0, layer = 0, access = 0, format = 0;
    };
    struct Blend {
        GLboolean enabled = GL_FALSE;
        GLint src_rgb = 0, dst_rgb = 0, src_alpha = 0, dst_alpha = 0, eq_rgb = 0, eq_alpha = 0;
        std::array<GLboolean, 4> mask{};
    };
    struct Vertex {
        GLint name = 0, stride = 0, divisor = 0;
        GLint64 offset = 0;
    };
    struct Attribute {
        GLint enabled = 0, size = 0, type = 0, normalized = 0, integer = 0, wide = 0;
        GLint binding = 0, relative_offset = 0;
    };
    struct VaoState {
        GLint element = 0;
        std::vector<Vertex> vertices;
        std::vector<Attribute> attributes;
    };
    struct Attachment {
        GLenum point = 0;
        GLint type = GL_NONE, name = 0, level = 0, layer = 0, layered = 0, face = 0;
    };
    struct FboState {
        std::vector<Attachment> attachments;
        std::vector<GLenum> draws;
        GLint read = 0;
        bool layer_textures = false;
    };
    struct Uniform {
        GLint location = -1;
        GLenum type = 0;
        std::array<GLfloat, 16> f{};
        std::array<GLdouble, 16> d{};
        std::array<GLint, 4> i{};
        std::array<GLuint, 4> u{};
    };
    ReplayObjectJournal<VaoState> vaos_;
    ReplayObjectJournal<FboState> fbos_;
    ReplayObjectJournal<std::vector<Uniform>> programs_;
    GLint draw_ = 0, read_ = 0, program_ = 0, vao_ = 0, active_ = 0, read_buffer_ = 0;
    GLint depth_func_ = 0, cull_ = 0, front_ = 0, clip_origin_ = 0, clip_depth_ = 0;
    GLint unpack_ = 0, parameter_ = 0;
    bool has_parameter_buffer_ = false;
    static constexpr GLenum kParameterBuffer = 0x80EE, kParameterBinding = 0x80EF;
    static constexpr std::array<GLenum, 8> kUnpackNames = {GL_UNPACK_ALIGNMENT,
                                                           GL_UNPACK_ROW_LENGTH,
                                                           GL_UNPACK_SKIP_ROWS,
                                                           GL_UNPACK_SKIP_PIXELS,
                                                           GL_UNPACK_IMAGE_HEIGHT,
                                                           GL_UNPACK_SKIP_IMAGES,
                                                           GL_UNPACK_SWAP_BYTES,
                                                           GL_UNPACK_LSB_FIRST};
    std::array<GLint, kUnpackNames.size()> unpack_values_{};
    std::array<GLint, 4> viewport_{}, scissor_{};
    std::array<GLint, 2> polygon_{};
    std::array<GLint, kBufferTargets.size()> buffers_{};
    std::array<GLboolean, kEnables.size()> enables_{};
    GLboolean depth_mask_ = GL_FALSE;
    GLdouble clear_depth_ = 0;
    std::array<GLdouble, 2> depth_range_{};
    std::array<GLfloat, 4> clear_color_{}, blend_color_{};
    GLfloat line_ = 1, offset_factor_ = 0, offset_units_ = 0;
    std::vector<GLenum> draw_buffers_;
    std::vector<Range> ssbo_, ubo_;
    std::vector<Texture> textures_;
    std::vector<Image> images_;
    std::vector<Blend> blends_;
    std::vector<Vertex> vertices_;
    std::vector<Attribute> attributes_;
    static void SaveRanges(std::vector<Range>& ranges, GLenum binding, GLenum start, GLenum size) {
        for (GLuint i = 0; i < ranges.size(); ++i) {
            glGetIntegeri_v(binding, i, &ranges[i].name);
            glGetInteger64i_v(start, i, &ranges[i].start);
            glGetInteger64i_v(size, i, &ranges[i].size);
        }
    }
    static void RestoreRanges(const std::vector<Range>& ranges, GLenum target) noexcept {
        for (GLuint i = 0; i < ranges.size(); ++i) {
            const auto& r = ranges[i];
            if (r.name && r.size > 0)
                glBindBufferRange(target, i, r.name, r.start, r.size);
            else
                glBindBufferBase(target, i, r.name);
        }
    }

    VaoState SaveVao(GLuint name) const {
        VaoState state;
        // Allocate before temporarily changing bindings; queries cannot throw.
        state.vertices.resize(vertices_.size());
        state.attributes.resize(attributes_.size());
        GLint previous = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous);
        glBindVertexArray(name);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state.element);
        for (GLuint i = 0; i < state.vertices.size(); ++i) {
            auto& v = state.vertices[i];
            glGetIntegeri_v(GL_VERTEX_BINDING_BUFFER, i, &v.name);
            glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, i, &v.stride);
            glGetIntegeri_v(GL_VERTEX_BINDING_DIVISOR, i, &v.divisor);
            glGetInteger64i_v(GL_VERTEX_BINDING_OFFSET, i, &v.offset);
        }
        for (GLuint i = 0; i < state.attributes.size(); ++i) {
            auto& a = state.attributes[i];
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &a.integer);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_LONG, &a.wide);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_BINDING, &a.binding);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_RELATIVE_OFFSET, &a.relative_offset);
        }
        glBindVertexArray(previous);
        return state;
    }
    static void RestoreVao(GLuint name, const VaoState& state) noexcept {
        glBindVertexArray(name);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.element);
        for (GLuint i = 0; i < state.attributes.size(); ++i) {
            const auto& a = state.attributes[i];
            if (a.wide)
                glVertexAttribLFormat(i, a.size, a.type, a.relative_offset);
            else if (a.integer)
                glVertexAttribIFormat(i, a.size, a.type, a.relative_offset);
            else
                glVertexAttribFormat(
                    i, a.size, a.type, static_cast<GLboolean>(a.normalized), a.relative_offset);
            glVertexAttribBinding(i, a.binding);
            if (a.enabled)
                glEnableVertexAttribArray(i);
            else
                glDisableVertexAttribArray(i);
        }
        for (GLuint i = 0; i < state.vertices.size(); ++i) {
            const auto& v = state.vertices[i];
            glBindVertexBuffer(i, v.name, v.offset, v.stride);
            glVertexBindingDivisor(i, v.divisor);
        }
    }
    FboState SaveFbo(GLuint name, bool layer_textures) const {
        FboState state;
        state.layer_textures = layer_textures;
        state.draws.resize(name ? blends_.size() : 1);
        GLint colors = 0;
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &colors);
        if (name)
            state.attachments.resize(colors + 2);
        GLint draw = 0, read = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glBindFramebuffer(GL_FRAMEBUFFER, name);
        glGetIntegerv(GL_READ_BUFFER, &state.read);
        for (GLuint i = 0; i < state.draws.size(); ++i) {
            GLint value = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
            state.draws[i] = value;
        }
        for (std::size_t i = 0; i < state.attachments.size(); ++i) {
            auto& a = state.attachments[i];
            a.point = i < static_cast<std::size_t>(colors)
                          ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i)
                      : i == static_cast<std::size_t>(colors) ? GL_DEPTH_ATTACHMENT
                                                              : GL_STENCIL_ATTACHMENT;
            const auto get = [&](GLenum query, GLint& value) {
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, a.point, query, &value);
            };
            get(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, a.type);
            if (a.type == GL_NONE)
                continue;
            get(GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, a.name);
            if (a.type != GL_TEXTURE)
                continue;
            get(GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, a.level);
            get(GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, a.layer);
            get(GL_FRAMEBUFFER_ATTACHMENT_LAYERED, a.layered);
            get(GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, a.face);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        return state;
    }
    static void RestoreFbo(GLuint name, const FboState& state) noexcept {
        glBindFramebuffer(GL_FRAMEBUFFER, name);
        for (const auto& a : state.attachments) {
            if (a.type == GL_RENDERBUFFER)
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, a.point, GL_RENDERBUFFER, a.name);
            else if (a.type == GL_NONE || a.layered)
                glFramebufferTexture(GL_FRAMEBUFFER, a.point, a.name, a.level);
            else if (state.layer_textures)
                glFramebufferTextureLayer(GL_FRAMEBUFFER, a.point, a.name, a.level, a.layer);
            else
                // All non-shadow attachments reachable from replay are 2D (or renderbuffers).
                glFramebufferTexture2D(
                    GL_FRAMEBUFFER, a.point, a.face ? a.face : GL_TEXTURE_2D, a.name, a.level);
        }
        glDrawBuffers(static_cast<GLsizei>(state.draws.size()), state.draws.data());
        glReadBuffer(state.read);
    }
    static std::vector<Uniform> SaveProgram(GLuint name) {
        GLint count = 0, maximum = 0;
        glGetProgramiv(name, GL_ACTIVE_UNIFORMS, &count);
        glGetProgramiv(name, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maximum);
        std::vector<char> text(static_cast<std::size_t>(maximum) + 1);
        std::vector<Uniform> uniforms;
        for (GLuint index = 0; index < static_cast<GLuint>(count); ++index) {
            GLint elements = 0;
            GLenum type = 0;
            glGetActiveUniform(name, index, maximum, nullptr, &elements, &type, text.data());
            const std::string base(text.data());
            const auto bracket = base.find("[0]");
            for (GLint element = 0; element < elements; ++element) {
                auto field = base;
                if (bracket != std::string::npos)
                    field.replace(bracket, 3, "[" + std::to_string(element) + "]");
                Uniform value;
                value.location = glGetUniformLocation(name, field.c_str());
                if (value.location < 0)
                    continue; // block members / atomic counters
                value.type = type;
                switch (type) {
                    case GL_FLOAT:
                    case GL_FLOAT_VEC2:
                    case GL_FLOAT_VEC3:
                    case GL_FLOAT_VEC4:
                    case GL_FLOAT_MAT2:
                    case GL_FLOAT_MAT3:
                    case GL_FLOAT_MAT4:
                    case GL_FLOAT_MAT2x3:
                    case GL_FLOAT_MAT2x4:
                    case GL_FLOAT_MAT3x2:
                    case GL_FLOAT_MAT3x4:
                    case GL_FLOAT_MAT4x2:
                    case GL_FLOAT_MAT4x3:
                        glGetUniformfv(name, value.location, value.f.data());
                        break;
                    case GL_DOUBLE:
                    case GL_DOUBLE_VEC2:
                    case GL_DOUBLE_VEC3:
                    case GL_DOUBLE_VEC4:
                    case GL_DOUBLE_MAT2:
                    case GL_DOUBLE_MAT3:
                    case GL_DOUBLE_MAT4:
                    case GL_DOUBLE_MAT2x3:
                    case GL_DOUBLE_MAT2x4:
                    case GL_DOUBLE_MAT3x2:
                    case GL_DOUBLE_MAT3x4:
                    case GL_DOUBLE_MAT4x2:
                    case GL_DOUBLE_MAT4x3:
                        glGetUniformdv(name, value.location, value.d.data());
                        break;
                    case GL_UNSIGNED_INT:
                    case GL_UNSIGNED_INT_VEC2:
                    case GL_UNSIGNED_INT_VEC3:
                    case GL_UNSIGNED_INT_VEC4:
                        glGetUniformuiv(name, value.location, value.u.data());
                        break;
                    default:
                        glGetUniformiv(name, value.location, value.i.data());
                        break;
                }
                uniforms.push_back(value);
            }
        }
        return uniforms;
    }
    static void RestoreProgram(GLuint name, const std::vector<Uniform>& uniforms) noexcept {
        glUseProgram(name);
        for (const auto& v : uniforms) {
            switch (v.type) {
                case GL_FLOAT:
                    glUniform1fv(v.location, 1, v.f.data());
                    break;
                case GL_FLOAT_VEC2:
                    glUniform2fv(v.location, 1, v.f.data());
                    break;
                case GL_FLOAT_VEC3:
                    glUniform3fv(v.location, 1, v.f.data());
                    break;
                case GL_FLOAT_VEC4:
                    glUniform4fv(v.location, 1, v.f.data());
                    break;
                case GL_DOUBLE:
                    glUniform1dv(v.location, 1, v.d.data());
                    break;
                case GL_DOUBLE_VEC2:
                    glUniform2dv(v.location, 1, v.d.data());
                    break;
                case GL_DOUBLE_VEC3:
                    glUniform3dv(v.location, 1, v.d.data());
                    break;
                case GL_DOUBLE_VEC4:
                    glUniform4dv(v.location, 1, v.d.data());
                    break;
                case GL_UNSIGNED_INT:
                    glUniform1uiv(v.location, 1, v.u.data());
                    break;
                case GL_UNSIGNED_INT_VEC2:
                    glUniform2uiv(v.location, 1, v.u.data());
                    break;
                case GL_UNSIGNED_INT_VEC3:
                    glUniform3uiv(v.location, 1, v.u.data());
                    break;
                case GL_UNSIGNED_INT_VEC4:
                    glUniform4uiv(v.location, 1, v.u.data());
                    break;
                case GL_INT:
                    glUniform1iv(v.location, 1, v.i.data());
                    break;
                case GL_INT_VEC2:
                    glUniform2iv(v.location, 1, v.i.data());
                    break;
                case GL_INT_VEC3:
                    glUniform3iv(v.location, 1, v.i.data());
                    break;
                case GL_INT_VEC4:
                    glUniform4iv(v.location, 1, v.i.data());
                    break;
                case GL_BOOL:
                    glUniform1iv(v.location, 1, v.i.data());
                    break;
                case GL_BOOL_VEC2:
                    glUniform2iv(v.location, 1, v.i.data());
                    break;
                case GL_BOOL_VEC3:
                    glUniform3iv(v.location, 1, v.i.data());
                    break;
                case GL_BOOL_VEC4:
                    glUniform4iv(v.location, 1, v.i.data());
                    break;
                case GL_FLOAT_MAT2:
                    glUniformMatrix2fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT2x3:
                    glUniformMatrix2x3fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT2x4:
                    glUniformMatrix2x4fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT3x2:
                    glUniformMatrix3x2fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT3:
                    glUniformMatrix3fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT3x4:
                    glUniformMatrix3x4fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT4x2:
                    glUniformMatrix4x2fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT4x3:
                    glUniformMatrix4x3fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_FLOAT_MAT4:
                    glUniformMatrix4fv(v.location, 1, GL_FALSE, v.f.data());
                    break;
                case GL_DOUBLE_MAT2:
                    glUniformMatrix2dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT2x3:
                    glUniformMatrix2x3dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT2x4:
                    glUniformMatrix2x4dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT3x2:
                    glUniformMatrix3x2dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT3:
                    glUniformMatrix3dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT3x4:
                    glUniformMatrix3x4dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT4x2:
                    glUniformMatrix4x2dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT4x3:
                    glUniformMatrix4x3dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                case GL_DOUBLE_MAT4:
                    glUniformMatrix4dv(v.location, 1, GL_FALSE, v.d.data());
                    break;
                default:
                    glUniform1iv(v.location, 1, v.i.data());
                    break; // samplers/images
            }
        }
    }

public:
    void PreserveObject(ReplayObjectKind kind, std::uint32_t name) override {
        switch (kind) {
            case ReplayObjectKind::VertexArray:
                if (name)
                    vaos_.Preserve(name, [this, name] { return SaveVao(name); });
                break;
            case ReplayObjectKind::Framebuffer:
            case ReplayObjectKind::LayerFramebuffer:
                fbos_.Preserve(name, [this, name, kind] {
                    return SaveFbo(name, kind == ReplayObjectKind::LayerFramebuffer);
                });
                break;
            case ReplayObjectKind::Program:
                if (name)
                    programs_.Preserve(name, [name] { return SaveProgram(name); });
                break;
        }
    }
    void Save() override {
        GLint n = 0;
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &n);
        ssbo_.resize(n);
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &n);
        ubo_.resize(n);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &n);
        textures_.resize(n);
        glGetIntegerv(GL_MAX_IMAGE_UNITS, &n);
        images_.resize(n);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &n);
        blends_.resize(n);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIB_BINDINGS, &n);
        vertices_.resize(n);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &n);
        attributes_.resize(n);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_);
        draw_buffers_.resize(draw_ ? blends_.size() : 1);
        // All allocations finish before changing the active texture selector.
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer_);
        for (GLuint i = 0; i < draw_buffers_.size(); ++i) {
            GLint value = 0;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
            draw_buffers_[i] = value;
        }
        glGetIntegerv(GL_VIEWPORT, viewport_.data());
        glGetIntegerv(GL_SCISSOR_BOX, scissor_.data());
        glGetIntegerv(GL_POLYGON_MODE, polygon_.data());
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear_depth_);
        glGetDoublev(GL_DEPTH_RANGE, depth_range_.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_.data());
        glGetFloatv(GL_BLEND_COLOR, blend_color_.data());
        glGetFloatv(GL_LINE_WIDTH, &line_);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset_factor_);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset_units_);
        glGetIntegerv(GL_CULL_FACE_MODE, &cull_);
        glGetIntegerv(GL_FRONT_FACE, &front_);
        glGetIntegerv(GL_CLIP_ORIGIN, &clip_origin_);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &clip_depth_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_);
        for (std::size_t i = 0; i < kUnpackNames.size(); ++i)
            glGetIntegerv(kUnpackNames[i], &unpack_values_[i]);
        // The optional parameter target is core in 4.6 / ARB_indirect_parameters.
        // Never query an unsupported enum on the engine's 4.3 contexts.
        GLint major = 0, minor = 0, extensions = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        has_parameter_buffer_ = major > 4 || (major == 4 && minor >= 6);
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
        for (GLint i = 0; !has_parameter_buffer_ && i < extensions; ++i) {
            const auto* extension = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            has_parameter_buffer_ =
                extension && std::string_view(reinterpret_cast<const char*>(extension)) ==
                                 "GL_ARB_indirect_parameters";
        }
        if (has_parameter_buffer_)
            glGetIntegerv(kParameterBinding, &parameter_);
        for (std::size_t i = 0; i < buffers_.size(); ++i)
            glGetIntegerv(kBufferBindings[i], &buffers_[i]);
        for (std::size_t i = 0; i < enables_.size(); ++i)
            enables_[i] = glIsEnabled(kEnables[i]);
        for (GLuint i = 0; i < blends_.size(); ++i) {
            auto& b = blends_[i];
            b.enabled = glIsEnabledi(GL_BLEND, i);
            glGetIntegeri_v(GL_BLEND_SRC_RGB, i, &b.src_rgb);
            glGetIntegeri_v(GL_BLEND_DST_RGB, i, &b.dst_rgb);
            glGetIntegeri_v(GL_BLEND_SRC_ALPHA, i, &b.src_alpha);
            glGetIntegeri_v(GL_BLEND_DST_ALPHA, i, &b.dst_alpha);
            glGetIntegeri_v(GL_BLEND_EQUATION_RGB, i, &b.eq_rgb);
            glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, i, &b.eq_alpha);
            glGetBooleani_v(GL_COLOR_WRITEMASK, i, b.mask.data());
        }
        SaveRanges(ssbo_,
                   GL_SHADER_STORAGE_BUFFER_BINDING,
                   GL_SHADER_STORAGE_BUFFER_START,
                   GL_SHADER_STORAGE_BUFFER_SIZE);
        SaveRanges(
            ubo_, GL_UNIFORM_BUFFER_BINDING, GL_UNIFORM_BUFFER_START, GL_UNIFORM_BUFFER_SIZE);
        if (vao_)
            for (GLuint i = 0; i < vertices_.size(); ++i) {
                auto& v = vertices_[i];
                glGetIntegeri_v(GL_VERTEX_BINDING_BUFFER, i, &v.name);
                glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, i, &v.stride);
                glGetIntegeri_v(GL_VERTEX_BINDING_DIVISOR, i, &v.divisor);
                glGetInteger64i_v(GL_VERTEX_BINDING_OFFSET, i, &v.offset);
            }
        if (vao_)
            for (GLuint i = 0; i < attributes_.size(); ++i) {
                auto& a = attributes_[i];
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &a.integer);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_LONG, &a.wide);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_BINDING, &a.binding);
                glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_RELATIVE_OFFSET, &a.relative_offset);
            }
        constexpr std::array<GLenum, 5> queries = {GL_TEXTURE_BINDING_2D,
                                                   GL_TEXTURE_BINDING_2D_ARRAY,
                                                   GL_TEXTURE_BINDING_3D,
                                                   GL_TEXTURE_BINDING_CUBE_MAP,
                                                   GL_TEXTURE_BINDING_BUFFER};
        for (GLuint i = 0; i < textures_.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            for (std::size_t j = 0; j < queries.size(); ++j)
                glGetIntegerv(queries[j], &textures_[i].names[j]);
            glGetIntegeri_v(GL_SAMPLER_BINDING, i, &textures_[i].sampler);
        }
        glActiveTexture(active_);
        for (GLuint i = 0; i < images_.size(); ++i) {
            auto& v = images_[i];
            glGetIntegeri_v(GL_IMAGE_BINDING_NAME, i, &v.name);
            glGetIntegeri_v(GL_IMAGE_BINDING_LEVEL, i, &v.level);
            glGetIntegeri_v(GL_IMAGE_BINDING_LAYERED, i, &v.layered);
            glGetIntegeri_v(GL_IMAGE_BINDING_LAYER, i, &v.layer);
            glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, i, &v.access);
            glGetIntegeri_v(GL_IMAGE_BINDING_FORMAT, i, &v.format);
        }
    }
    void Restore() noexcept override {
        // Object configurations first; restore the caller's context bindings last.
        vaos_.Restore(RestoreVao);
        fbos_.Restore(RestoreFbo);
        programs_.Restore(RestoreProgram);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_);
        glDrawBuffers(static_cast<GLsizei>(draw_buffers_.size()), draw_buffers_.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_);
        glReadBuffer(read_buffer_);
        glUseProgram(program_);
        glBindVertexArray(vao_);
        if (vao_)
            for (GLuint i = 0; i < attributes_.size(); ++i) {
                const auto& a = attributes_[i];
                if (a.wide)
                    glVertexAttribLFormat(i, a.size, a.type, a.relative_offset);
                else if (a.integer)
                    glVertexAttribIFormat(i, a.size, a.type, a.relative_offset);
                else
                    glVertexAttribFormat(
                        i, a.size, a.type, static_cast<GLboolean>(a.normalized), a.relative_offset);
                glVertexAttribBinding(i, a.binding);
                if (a.enabled)
                    glEnableVertexAttribArray(i);
                else
                    glDisableVertexAttribArray(i);
            }
        if (vao_)
            for (GLuint i = 0; i < vertices_.size(); ++i) {
                const auto& v = vertices_[i];
                glBindVertexBuffer(i, v.name, v.offset, v.stride);
                glVertexBindingDivisor(i, v.divisor);
            }
        RestoreRanges(ssbo_, GL_SHADER_STORAGE_BUFFER);
        RestoreRanges(ubo_, GL_UNIFORM_BUFFER);
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            if (kBufferTargets[i] != GL_ELEMENT_ARRAY_BUFFER || vao_)
                glBindBuffer(kBufferTargets[i], buffers_[i]);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_);
        if (has_parameter_buffer_)
            glBindBuffer(kParameterBuffer, parameter_);
        for (std::size_t i = 0; i < kUnpackNames.size(); ++i)
            glPixelStorei(kUnpackNames[i], unpack_values_[i]);
        constexpr std::array<GLenum, 5> targets = {GL_TEXTURE_2D,
                                                   GL_TEXTURE_2D_ARRAY,
                                                   GL_TEXTURE_3D,
                                                   GL_TEXTURE_CUBE_MAP,
                                                   GL_TEXTURE_BUFFER};
        for (GLuint i = 0; i < textures_.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            for (std::size_t j = 0; j < targets.size(); ++j)
                glBindTexture(targets[j], textures_[i].names[j]);
            glBindSampler(i, textures_[i].sampler);
        }
        glActiveTexture(active_);
        for (GLuint i = 0; i < images_.size(); ++i) {
            const auto& v = images_[i];
            glBindImageTexture(
                i, v.name, v.level, static_cast<GLboolean>(v.layered), v.layer, v.access, v.format);
        }
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        for (std::size_t i = 0; i < enables_.size(); ++i) {
            if (enables_[i])
                glEnable(kEnables[i]);
            else
                glDisable(kEnables[i]);
        }
        for (GLuint i = 0; i < blends_.size(); ++i) {
            const auto& b = blends_[i];
            if (b.enabled)
                glEnablei(GL_BLEND, i);
            else
                glDisablei(GL_BLEND, i);
            glBlendFuncSeparatei(i, b.src_rgb, b.dst_rgb, b.src_alpha, b.dst_alpha);
            glBlendEquationSeparatei(i, b.eq_rgb, b.eq_alpha);
            glColorMaski(i, b.mask[0], b.mask[1], b.mask[2], b.mask[3]);
        }
        glDepthFunc(depth_func_);
        glDepthMask(depth_mask_);
        glClearDepth(clear_depth_);
        glDepthRange(depth_range_[0], depth_range_[1]);
        glClearColor(clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]);
        glBlendColor(blend_color_[0], blend_color_[1], blend_color_[2], blend_color_[3]);
        // Core profile accepts FRONT_AND_BACK only; both modes are equal there.
        glPolygonMode(GL_FRONT_AND_BACK, polygon_[0]);
        glLineWidth(line_);
        glPolygonOffset(offset_factor_, offset_units_);
        glCullFace(cull_);
        glFrontFace(front_);
        glClipControl(clip_origin_, clip_depth_);
    }
};
} // namespace
std::unique_ptr<ReplayStateAccess> MakeFarWorldReplayGlAccess() {
    return std::make_unique<GlAccess>();
}
} // namespace Luminumbra::Rendering
