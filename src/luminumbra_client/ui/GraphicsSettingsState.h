#pragma once

#include "luminumbra_common/core/GraphicsPresetClassifier.h"
#include "luminumbra_common/core/GraphicsPresets.h"

#include <optional>
#include <string>
#include <utility>

namespace Luminumbra::Client {

enum class GraphicsEditKind {
    Preset,
    Reset,
    Override,
    DetectApply
};

struct GraphicsEdit {
    GraphicsEditKind kind = GraphicsEditKind::Reset;
    luminumbra::core::GraphicsPreset preset = luminumbra::core::GraphicsPreset::Ultra;
    std::string key;
    luminumbra::core::GraphicsOverrideValue value = 1.0;
};

inline std::string GraphicsHardwareSummary(const luminumbra::core::GraphicsHardwareInputs& inputs) {
    // Driver strings are bounded before display/persistence, including fixture inputs.
    return "GPU: " + inputs.renderer.substr(0, luminumbra::core::kMaxGraphicsIdentityBytes) +
           " | CPU threads: " + std::to_string(inputs.logicalProcessorCount) + " | VRAM: " +
           (inputs.dedicatedVideoMemoryBytes
                ? std::to_string(*inputs.dedicatedVideoMemoryBytes / (1024 * 1024)) + " MiB"
                : "unknown");
}

// Pure reduction. Detection inputs must be freshly collected by the context-thread host.
// Invalid requests leave the result untouched, including invalid preset enum values.
inline bool
ReduceGraphicsSettings(const luminumbra::core::GraphicsSettings& current,
                       const GraphicsEdit& edit,
                       const std::optional<luminumbra::core::GraphicsHardwareInputs>& detected,
                       luminumbra::core::GraphicsSettings& result,
                       std::string& error) {
    using namespace luminumbra::core;
    auto candidate = current;
    switch (edit.kind) {
        case GraphicsEditKind::Preset:
            if (!SelectGraphicsPreset(candidate, edit.preset)) {
                error = "Invalid graphics preset";
                return false;
            }
            break;
        case GraphicsEditKind::Reset:
            ResetGraphicsOverrides(candidate);
            break;
        case GraphicsEditKind::Override:
            // A validated map has at most nine keys; allow one bounded candidate key.
            // The only string value is the four-byte blur enum.
            if (edit.key.size() > 32 || (std::holds_alternative<std::string>(edit.value) &&
                                         std::get<std::string>(edit.value).size() > 4)) {
                error = "Invalid graphics setting";
                return false;
            }
            candidate.overrides[edit.key] = edit.value;
            break;
        case GraphicsEditKind::DetectApply: {
            if (!detected) {
                error = "Hardware detection unavailable";
                return false;
            }
            const auto recommendation = ClassifyGraphicsPreset(*detected);
            if (!SelectGraphicsPreset(candidate, recommendation.preset))
                return false;
            candidate.detection = {.observed_hardware = GraphicsHardwareSummary(*detected),
                                   .rules_revision = recommendation.rulesRevision,
                                   .recommended_preset = recommendation.preset};
            break;
        }
        default:
            error = "Invalid graphics action";
            return false;
    }
    ResolvedGraphics resolved;
    if (!ResolveGraphics(candidate, {}, resolved, error))
        return false;
    result = std::move(candidate);
    error.clear();
    return true;
}

struct GraphicsSettingsDisplay {
    luminumbra::core::ResolvedGraphics effective;
    std::string preset_label;
    std::string hardware;
    std::string status;
};

// Pure display projection: Custom retains the saved base; temporary effective values are marked.
inline GraphicsSettingsDisplay
BuildGraphicsSettingsDisplay(const luminumbra::core::GraphicsSettings& settings,
                             const luminumbra::core::GraphicsOverrides& session,
                             const std::string& status) {
    using namespace luminumbra::core;
    GraphicsSettingsDisplay display;
    std::string error;
    if (!ResolveGraphics(settings, session, display.effective, error)) {
        display.status = error;
        return display;
    }
    const std::string base(GraphicsPresetDisplayName(settings.base_preset));
    display.preset_label = display.effective.custom ? "Custom (base: " + base + ")" : base;
    if (!session.empty())
        display.preset_label += " | Session overrides active; controls show effective values";
    if (settings.detection.rules_revision != 0) {
        display.hardware =
            settings.detection.observed_hardware + " | Recommended: " +
            std::string(GraphicsPresetDisplayName(settings.detection.recommended_preset));
    }
    display.status = status;
    return display;
}

// Context-thread transaction state, with explicit time supplied by the host for deterministic
// debounce tests. No renderer, environment reads, filesystem operations or callbacks.
class GraphicsSettingsState {
public:
    GraphicsSettingsState(luminumbra::core::GraphicsSettings settings,
                          luminumbra::core::GraphicsOverrides session,
                          std::string status = {})
        : m_settings(std::move(settings))
        , m_session(std::move(session))
        , m_status(std::move(status)) {}

    bool Stage(const GraphicsEdit& edit,
               const std::optional<luminumbra::core::GraphicsHardwareInputs>& detected = {}) {
        luminumbra::core::GraphicsSettings candidate;
        std::string error;
        if (!ReduceGraphicsSettings(
                m_pending ? *m_pending : m_settings, edit, detected, candidate, error)) {
            m_status = std::move(error);
            return false;
        }
        luminumbra::core::ResolvedGraphics effective;
        if (!luminumbra::core::ResolveGraphics(candidate, m_session, effective, error)) {
            m_status = std::move(error);
            return false;
        }
        m_pending = std::move(candidate);
        m_apply_error.clear();
        m_status = "Applying graphics";
        return true;
    }

    const std::optional<luminumbra::core::GraphicsSettings>& Pending() const {
        return m_pending;
    }
    const luminumbra::core::GraphicsSettings& Settings() const {
        return m_settings;
    }
    void Applied(bool success, double now) {
        if (!m_pending)
            return;
        if (success) {
            m_settings = std::move(*m_pending);
            ScheduleSave(now);
        } else {
            m_status.clear();
            m_apply_error = "Graphics apply failed; previous values retained";
        }
        m_pending.reset();
    }

    void ScheduleSave(double now) {
        m_save_pending = true;
        m_save_at = now + 0.5;
        m_status = "Graphics applied; waiting to save";
    }
    bool SaveDue(double now) const {
        return m_save_pending && !m_pending && now >= m_save_at;
    }
    bool SavePending() const {
        return m_save_pending;
    }
    void Saved(bool success) {
        m_save_pending = false; // Explicit retry or another edit can schedule a new attempt.
        m_status = success ? "Graphics saved" : "Save failed; graphics are active for this session";
    }
    GraphicsSettingsDisplay Display() const {
        const auto status = m_status + (m_apply_error.empty() ? ""
                                        : m_status.empty()    ? m_apply_error
                                                              : " | " + m_apply_error);
        return BuildGraphicsSettingsDisplay(m_pending ? *m_pending : m_settings, m_session, status);
    }

private:
    luminumbra::core::GraphicsSettings m_settings;
    luminumbra::core::GraphicsOverrides m_session;
    std::optional<luminumbra::core::GraphicsSettings> m_pending;
    std::string m_status;
    std::string m_apply_error;
    bool m_save_pending = false;
    double m_save_at = 0.0;
};

} // namespace Luminumbra::Client
