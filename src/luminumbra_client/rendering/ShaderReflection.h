#pragma once

#include <glad/glad.h>

#include <string>
#include <string_view>
#include <vector>

// shader-resource REFLECTION + layout validation.
//
// At program-link time we introspect the linked GL program's declared resource
// layout -- the samplers it actually uses (name + GL type + default texture unit),
// the uniform / shader-storage blocks (name + binding point), and the fragment
// outputs (name + location). A pass then declares the bindings it ADOPTS for that
// shader (the units/types it will glActiveTexture+glBindTexture into) and we
// validate that declaration against the reflected layout. A mismatch -- a missing
// sampler, a wrong binding point, or (the "renders garbage" bug) a TYPE mismatch
// such as binding a sampler2DArray where the shader declares sampler2D -- is
// reported loudly instead of silently producing a corrupt frame.
//
// ReflectedLayout is backend-neutral: linked OpenGL introspection and compiled
// shader reflection both produce the same validation input.
//
// Reflection-only: every call here is a read-only GL program query
// (glGetProgramInterfaceiv / glGetProgramResource*) plus glGetUniformiv to read a
// sampler's post-link unit. None of it touches framebuffer/pipeline state, so it
// is pixel-neutral by construction.

namespace Luminumbra::Rendering {

// --- Reflected (introspected) layout ----------------------------------------

struct ReflectedSampler {
    std::string name;    // active uniform name (e.g. "u_shadowCascades")
    GLenum type = 0;     // GL_SAMPLER_2D / GL_SAMPLER_2D_ARRAY / GL_SAMPLER_CUBE /...
    GLint location = -1; // glGetUniformLocation (for reading the unit)
    GLint unit = 0;      // post-link sampler value: layout(binding=) default, else 0
};

struct ReflectedBlock {
    std::string name;  // block name (e.g. "ScatterParams")
    GLint binding = 0; // GL_BUFFER_BINDING (the layout(binding=) point)
};

struct ReflectedOutput {
    std::string name;    // fragment output name (e.g. "FragColor")
    GLint location = -1; // GL_LOCATION (the draw-buffer / attachment index)
};

struct ReflectedLayout {
    std::vector<ReflectedSampler> samplers;
    std::vector<ReflectedBlock> uniform_blocks;
    std::vector<ReflectedBlock> storage_blocks;
    std::vector<ReflectedOutput> outputs;

    const ReflectedSampler* find_sampler(std::string_view name) const;
    const ReflectedBlock* find_uniform_block(std::string_view name) const;
    const ReflectedBlock* find_storage_block(std::string_view name) const;
};

// Introspect a successfully-linked GL program. Safe to call on program 0 (returns
// an empty layout). Requires GL 4.3+ program-interface query (the engine is 4.5).
ReflectedLayout ReflectProgramLayout(GLuint program);

// --- Expected (pass-declared) layout -----------------------------------------

struct ExpectedSampler {
    std::string name; // sampler the pass binds into
    GLenum type = 0;  // required GL sampler type (the garbage-prevention check)
    int unit = -1;    // <0 => don't check the unit; >=0 => require reflected unit == this
};

struct ExpectedBlock {
    std::string name;
    int binding = -1; // <0 => don't check; >=0 => require reflected binding == this
};

struct ExpectedLayout {
    std::string pass_name; // for diagnostics ("lighting", "skybox",...)
    std::vector<ExpectedSampler> samplers;
    std::vector<ExpectedBlock> uniform_blocks;
    std::vector<ExpectedBlock> storage_blocks;
};

struct ValidationResult {
    bool ok = true;           // false only on a HARD mismatch (type / binding-point)
    bool had_warning = false; // a soft issue (an expected sampler the linker stripped)
    std::string diagnostic;   // human-readable; empty when fully clean
};

// Validate a pass's expected layout against the reflected layout.
//
// Hard failure (ok=false): an expected sampler/block is PRESENT in the program but
//   has the wrong GL type or a wrong binding/unit. This is the "renders garbage"
//   class and the load-time tripwire   targets.
// Soft warning (had_warning=true, ok stays true): an expected sampler is ABSENT.
//   The GL linker strips declared-but-unused uniforms, so absence alone is not
//   proof of a real bug -- we surface it but do not fail the program.
ValidationResult ValidateReflectedLayout(const ReflectedLayout& reflected,
                                         const ExpectedLayout& expected);

// Human-readable GL sampler/type name for diagnostics (falls back to hex).
std::string GlTypeName(GLenum type);

// True for the GL_SAMPLER_* / GL_INT_SAMPLER_* / GL_UNSIGNED_INT_SAMPLER_* family.
bool IsSamplerType(GLenum type);

} // namespace Luminumbra::Rendering
