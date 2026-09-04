#include "GlDebugOutput.h"

#include "core/Log.h"
#include "luminumbra_common/core/Environment.h"

#include <atomic>

namespace Luminumbra::Rendering::GlDebug {

namespace {

// Install state + counters. Atomics because the driver may invoke the callback
// from a non-render thread on async (non-SYNCHRONOUS) contexts; we force
// SYNCHRONOUS so in practice it is the render thread, but the atomics keep the
// telemetry race-free regardless and cost nothing on the hot path.
// The driver callback and installer run independently, so state must survive both call stacks.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_installed{false};
// Driver callbacks update process-lifetime telemetry read outside any one callback invocation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned long long> g_total{0};
// Driver callbacks update process-lifetime telemetry read outside any one callback invocation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned long long> g_errors{0};
// Driver callbacks update process-lifetime telemetry read outside any one callback invocation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned long long> g_warnings{0};
// Driver callbacks update process-lifetime telemetry read outside any one callback invocation.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned long long> g_notifications{0};

const char* source_str(GLenum source) {
    switch (source) {
        case GL_DEBUG_SOURCE_API:
            return "API";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
            return "WindowSystem";
        case GL_DEBUG_SOURCE_SHADER_COMPILER:
            return "ShaderCompiler";
        case GL_DEBUG_SOURCE_THIRD_PARTY:
            return "ThirdParty";
        case GL_DEBUG_SOURCE_APPLICATION:
            return "Application";
        case GL_DEBUG_SOURCE_OTHER:
            return "Other";
        default:
            return "Unknown";
    }
}

const char* type_str(GLenum type) {
    switch (type) {
        case GL_DEBUG_TYPE_ERROR:
            return "Error";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            return "Deprecated";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            return "UndefinedBehavior";
        case GL_DEBUG_TYPE_PORTABILITY:
            return "Portability";
        case GL_DEBUG_TYPE_PERFORMANCE:
            return "Performance";
        case GL_DEBUG_TYPE_MARKER:
            return "Marker";
        case GL_DEBUG_TYPE_PUSH_GROUP:
            return "PushGroup";
        case GL_DEBUG_TYPE_POP_GROUP:
            return "PopGroup";
        case GL_DEBUG_TYPE_OTHER:
            return "Other";
        default:
            return "Unknown";
    }
}

const char* severity_str(GLenum severity) {
    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
            return "HIGH";
        case GL_DEBUG_SEVERITY_MEDIUM:
            return "MEDIUM";
        case GL_DEBUG_SEVERITY_LOW:
            return "LOW";
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            return "NOTIFICATION";
        default:
            return "?";
    }
}

void GLAPIENTRY debug_message_router(GLenum source,
                                     GLenum type,
                                     GLuint id,
                                     GLenum severity,
                                     GLsizei length,
                                     const GLchar* message,
                                     const void* user_param) {
    (void)length;
    (void)user_param;

    g_total.fetch_add(1, std::memory_order_relaxed);

    // An ERROR type, or anything the driver flags HIGH, is a real fault: surface it
    // as an error regardless of the message type.: we only log it.
    const bool is_error = (type == GL_DEBUG_TYPE_ERROR) || (severity == GL_DEBUG_SEVERITY_HIGH);
    if (is_error) {
        g_errors.fetch_add(1, std::memory_order_relaxed);
        LUMINUMBRA_CORE_ERROR("[GL][{}][{}][{}] id={} {}",
                              source_str(source),
                              type_str(type),
                              severity_str(severity),
                              id,
                              message ? message : "");
        return;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            // Suppressed at the driver level by default (glDebugMessageControl), so
            // this branch only runs in LUMIN_GL_DEBUG=verbose mode. Keep it quiet (trace).
            g_notifications.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_TRACE("[GL][{}][{}][NOTIFICATION] id={} {}",
                                  source_str(source),
                                  type_str(type),
                                  id,
                                  message ? message : "");
            break;
        default: // LOW / MEDIUM warnings, perf tips, deprecation, portability.
            g_warnings.fetch_add(1, std::memory_order_relaxed);
            LUMINUMBRA_CORE_WARN("[GL][{}][{}][{}] id={} {}",
                                 source_str(source),
                                 type_str(type),
                                 severity_str(severity),
                                 id,
                                 message ? message : "");
            break;
    }
}

} // namespace

bool InstallGlDebugCallback() {
    if (g_installed.load(std::memory_order_acquire)) {
        return true; // idempotent
    }

    // Opt-in gate: default-OFF because SYNCHRONOUS debug output is slow.
    const auto flag = Core::ReadEnvironment("LUMIN_GL_DEBUG");
    if (!flag || flag->empty() || *flag == "0") {
        return false;
    }
    const bool verbose = (*flag == "verbose");

    // KHR_debug entry points are core in 4.3+. Guard on non-null anyway: a context
    // created without the debug bit (or an exotic loader) may leave them null, and
    // we must degrade to a silent no-op rather than crash.
#ifdef GL_VERSION_4_3
    if (glDebugMessageCallback == nullptr || glDebugMessageControl == nullptr) {
        LUMINUMBRA_CORE_WARN("LUMIN_GL_DEBUG set but KHR_debug entry points are unavailable; GL "
                             "debug output disabled");
        return false;
    }

    glEnable(GL_DEBUG_OUTPUT);
    // SYNCHRONOUS: deliver each message on the callstack of the offending GL call so
    // the logged location is actionable (and so a breakpoint in the router lands on
    // the culprit). This is the slow part — hence the env gate.
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(&debug_message_router, nullptr);

    // Default filter: enable everything, then suppress NOTIFICATION severity unless
    // running verbose. NOTIFICATION is mostly buffer-residency / redundant-state spam.
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    if (!verbose) {
        glDebugMessageControl(
            GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
    }

    g_installed.store(true, std::memory_order_release);
    LUMINUMBRA_CORE_INFO(
        "GL KHR_debug output installed (synchronous{}); routing driver messages to the engine log",
        verbose ? ", verbose" : ", notifications suppressed");
    return true;
#else
    (void)verbose;
    LUMINUMBRA_CORE_WARN(
        "LUMIN_GL_DEBUG set but build lacks GL 4.3 KHR_debug headers; GL debug output disabled");
    return false;
#endif
}

bool IsGlDebugInstalled() {
    return g_installed.load(std::memory_order_acquire);
}

GlDebugCounters GetGlDebugCounters() {
    GlDebugCounters c;
    c.total = g_total.load(std::memory_order_relaxed);
    c.errors = g_errors.load(std::memory_order_relaxed);
    c.warnings = g_warnings.load(std::memory_order_relaxed);
    c.notifications = g_notifications.load(std::memory_order_relaxed);
    return c;
}

void LabelGlObject(GLenum identifier, GLuint name, const std::string& label) {
    if (name == 0) {
        return;
    }
#ifdef GL_VERSION_4_3
    if (glObjectLabel) {
        glObjectLabel(identifier, name, -1, label.c_str());
    }
#else
    (void)identifier;
    (void)label;
#endif
}

GlDebugGroup::GlDebugGroup(const std::string& label) {
#ifdef GL_VERSION_4_3
    if (glPushDebugGroup) {
        // GL_DEBUG_SOURCE_APPLICATION + a fixed id keeps these out of the driver's
        // own marker namespace. length=-1 => null-terminated string.
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0u, -1, label.c_str());
        m_pushed = true;
    }
#else
    (void)label;
#endif
}

GlDebugGroup::~GlDebugGroup() {
#ifdef GL_VERSION_4_3
    if (m_pushed && glPopDebugGroup) {
        glPopDebugGroup();
    }
#endif
}

} // namespace Luminumbra::Rendering::GlDebug
