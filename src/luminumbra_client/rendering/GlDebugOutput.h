#pragma once

// =============================================================================
// GlDebugOutput — KHR_debug (core GL 4.3+, we run 4.5) diagnostic plumbing.
// =============================================================================
//  / DIAGNOSTIC. None of this touches sim state, the registry, or the
// world_hash. Installing the debug callback only changes how the *driver* reports
// errors/warnings to us (it routes them to the engine logger); it never alters a
// rendered pixel and never feeds determinism. The whole module is a no-op unless
// the player opts in via the LUMIN_GL_DEBUG=1 environment variable, because
// GL_DEBUG_OUTPUT_SYNCHRONOUS forces a CPU/GPU sync at every GL call and is far
// too slow to leave on by default.
//
// What you get:
//   InstallGlDebugCallback  — call once right after gladLoadGL / context init.
//                               Enables GL_DEBUG_OUTPUT (+ SYNCHRONOUS so the log
//                               stack trace points at the offending call), routes
//                               driver messages to LUMINUMBRA_CORE_* with severity
//                               filtering (NOTIFICATION suppressed by default), and
//                               primes the message filter. Gated on LUMIN_GL_DEBUG=1.
//   GlDebugGroup              — tiny RAII glPushDebugGroup/glPopDebugGroup marker so
//                               RenderDoc / Nsight captures show named per-pass spans.
//   LabelGlObject(...)        — glObjectLabel wrapper so textures/FBOs/buffers show
//                               human names in capture tools instead of bare ids.
//
// Everything degrades to a silent no-op on contexts that lack KHR_debug (the entry
// points are null) or when the install was never requested, so it is safe to call
// the markers unconditionally from the render passes.

#include <glad/glad.h>

#include <string>

namespace Luminumbra::Rendering::GlDebug {

// Install the KHR_debug message callback, gated behind LUMIN_GL_DEBUG=1.
//
// Returns true if the callback was installed (env opt-in present, context exposes
// KHR_debug, entry points non-null); false if it was skipped (the common, default
// path — env not set, or no KHR_debug). Safe and cheap to call exactly once,
// immediately after gladLoadGL(Loader) in main_client. Idempotent: a second call
// is a no-op once installed.
//
// When installed it:
//   * glEnable(GL_DEBUG_OUTPUT) and glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS) so each
//     message is delivered on the thread/callstack of the GL call that caused it.
//   * glDebugMessageCallback(...) -> the logger router below.
//   * Suppresses GL_DEBUG_SEVERITY_NOTIFICATION (and a few chatty NVIDIA buffer-
//     residency / redundant-state ids) via glDebugMessageControl, unless the env
//     value is "verbose" (LUMIN_GL_DEBUG=verbose) which keeps notifications.
bool InstallGlDebugCallback();

// True once InstallGlDebugCallback has actually installed the callback. Lets
// callers cheaply skip building debug-group label strings when nothing consumes them.
bool IsGlDebugInstalled();

// Cumulative message counters since install (diagnostic; render-only telemetry).
// Useful for a "GL emitted N errors this run" health line / FrameScan-style report.
struct GlDebugCounters {
    unsigned long long total = 0;
    unsigned long long errors = 0;
    unsigned long long warnings = 0;
    unsigned long long notifications = 0;
};
GlDebugCounters GetGlDebugCounters();

// ---------------------------------------------------------------------------
// LabelGlObject — name a GL object for capture tools (no-op pre-4.3 / null entry).
// identifier is the GL namespace enum: GL_TEXTURE, GL_FRAMEBUFFER, GL_BUFFER,
// GL_VERTEX_ARRAY, GL_PROGRAM, GL_RENDERBUFFER, GL_QUERY,... `name` is the id.
// ---------------------------------------------------------------------------
void LabelGlObject(GLenum identifier, GLuint name, const std::string& label);

// ---------------------------------------------------------------------------
// GlDebugGroup — RAII glPushDebugGroup on construct, glPopDebugGroup on destruct.
// Scope it at the top of a render pass; the matching pop is guaranteed even if the
// pass early-returns or throws. No-op when KHR_debug is unavailable, so it is free
// to leave in always-compiled code. Non-copyable / non-movable (the GL group stack
// is positional — a moved-from pop would unbalance the stack).
//
//   {
//       GlDebugGroup _grp("GBuffer");
//       m_gbuffer_pass->execute(...);
//   } // auto-pop
// ---------------------------------------------------------------------------
class GlDebugGroup {
public:
    explicit GlDebugGroup(const std::string& label);
    ~GlDebugGroup();

    GlDebugGroup(const GlDebugGroup&) = delete;
    GlDebugGroup& operator=(const GlDebugGroup&) = delete;
    GlDebugGroup(GlDebugGroup&&) = delete;
    GlDebugGroup& operator=(GlDebugGroup&&) = delete;

private:
    bool m_pushed = false;
};

} // namespace Luminumbra::Rendering::GlDebug
