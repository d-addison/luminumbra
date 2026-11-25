#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace luminumbra::core {

enum class MetricAvailability {
    Available,
    Pending,
    Unsupported,
    Unavailable,
    Stale
};

inline const char* MetricAvailabilityName(MetricAvailability availability) {
    switch (availability) {
        case MetricAvailability::Available:
            return "available";
        case MetricAvailability::Pending:
            return "pending";
        case MetricAvailability::Unsupported:
            return "unsupported";
        case MetricAvailability::Unavailable:
            return "unavailable";
        case MetricAvailability::Stale:
            return "stale";
    }
    return "unavailable";
}

struct InspectorMetricDefinition {
    std::string_view id;
    std::string_view label;
    std::string_view source;
    std::string_view unit;
    std::uint64_t max_age_frames = 0;
};

struct InspectorMetricSample {
    MetricAvailability availability = MetricAvailability::Pending;
    std::optional<double> value = {};
    // Producer frame in the caller's observation sequence; absent means unknown age.
    std::optional<std::uint64_t> sampled_frame = {};
};

// Fixed storage bounds all registration, publication and query work: eight panels,
// eight metrics per panel, and at most eighty bytes per metadata field. No query
// allocates, calls a provider, reads authority, or waits for a GPU query.
inline constexpr std::size_t kInspectorPanelLimit = 8;
inline constexpr std::size_t kInspectorMetricLimit = 8;
inline constexpr std::size_t kInspectorTextLimit = 80;
using InspectorText = std::array<char, kInspectorTextLimit + 1>;

struct InspectorMetricObservation {
    InspectorText id{};
    InspectorText label{};
    InspectorText source{};
    InspectorText unit{};
    MetricAvailability availability = MetricAvailability::Pending;
    std::optional<double> value = {};
    std::optional<std::uint64_t> age_frames = {};
};

struct InspectorPanelSnapshot {
    InspectorText id{};
    InspectorText title{};
    std::size_t metric_count = 0;
    std::array<InspectorMetricObservation, kInspectorMetricLimit> metrics{};
};

class InspectorRegistry;
struct InspectorPanelHandle {
    const InspectorRegistry* owner = nullptr;
    std::size_t slot = kInspectorPanelLimit;
    std::uint64_t generation = 0;
};

struct InspectorPanelList {
    std::array<InspectorPanelHandle, kInspectorPanelLimit> panels{};
    std::size_t count = 0;
};

// Single-owner-thread service. Publish copies already completed observations on
// that thread; consumers receive independent values, never live subsystem pointers.
// Definitions are immutable until unregister/shutdown. Unavailable samples carry
// no numeric value; unknown ages stay unknown, and stale values are withheld.
class InspectorRegistry {
public:
    InspectorRegistry() = default;
    InspectorRegistry(const InspectorRegistry&) = delete;
    InspectorRegistry& operator=(const InspectorRegistry&) = delete;
    InspectorRegistry(InspectorRegistry&&) = delete;
    InspectorRegistry& operator=(InspectorRegistry&&) = delete;

    std::optional<InspectorPanelHandle>
    Register(std::string_view id,
             std::string_view title,
             std::span<const InspectorMetricDefinition> definitions) {
        if (!ValidText(id) || !ValidText(title) || definitions.empty() ||
            definitions.size() > kInspectorMetricLimit ||
            m_generation == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            const auto& definition = definitions[index];
            if (!ValidText(definition.id) || !ValidText(definition.label) ||
                !ValidText(definition.source) || !ValidText(definition.unit)) {
                return std::nullopt;
            }
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (definitions[previous].id == definition.id) {
                    return std::nullopt;
                }
            }
        }
        std::size_t free_slot = kInspectorPanelLimit;
        for (std::size_t index = 0; index < m_panels.size(); ++index) {
            if (m_panels[index].generation == 0 && free_slot == kInspectorPanelLimit) {
                free_slot = index;
            } else if (std::string_view(m_panels[index].snapshot.id.data()) == id) {
                return std::nullopt;
            }
        }
        if (free_slot == kInspectorPanelLimit) {
            return std::nullopt;
        }
        auto& panel = m_panels[free_slot];
        CopyText(panel.snapshot.id, id);
        CopyText(panel.snapshot.title, title);
        panel.snapshot.metric_count = definitions.size();
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            const auto& definition = definitions[index];
            auto& metric = panel.snapshot.metrics[index];
            CopyText(metric.id, definition.id);
            CopyText(metric.label, definition.label);
            CopyText(metric.source, definition.source);
            CopyText(metric.unit, definition.unit);
            panel.max_age_frames[index] = definition.max_age_frames;
        }
        panel.generation = ++m_generation;
        return InspectorPanelHandle{this, free_slot, panel.generation};
    }

    bool Publish(InspectorPanelHandle handle, std::span<const InspectorMetricSample> samples) {
        if (!Contains(handle) || samples.size() != m_panels[handle.slot].snapshot.metric_count) {
            return false;
        }
        for (const auto& sample : samples) {
            switch (sample.availability) {
                case MetricAvailability::Available:
                    if (!sample.value || !std::isfinite(*sample.value)) {
                        return false;
                    }
                    break;
                case MetricAvailability::Pending:
                case MetricAvailability::Unsupported:
                case MetricAvailability::Unavailable:
                    if (sample.value) {
                        return false;
                    }
                    break;
                default:
                    return false;
            }
        }
        std::copy(samples.begin(), samples.end(), m_panels[handle.slot].samples.begin());
        return true;
    }

    std::optional<InspectorPanelSnapshot> Read(InspectorPanelHandle handle,
                                               std::uint64_t current_frame) const {
        if (!Contains(handle)) {
            return std::nullopt;
        }
        const auto& panel = m_panels[handle.slot];
        auto snapshot = panel.snapshot;
        for (std::size_t index = 0; index < snapshot.metric_count; ++index) {
            auto& metric = snapshot.metrics[index];
            const auto& sample = panel.samples[index];
            metric.availability = sample.availability;
            metric.value = sample.value;
            if (sample.sampled_frame) {
                if (*sample.sampled_frame > current_frame) {
                    metric.availability = MetricAvailability::Unavailable;
                    metric.value.reset();
                    continue;
                }
                metric.age_frames = current_frame - *sample.sampled_frame;
                if (metric.availability == MetricAvailability::Available &&
                    *metric.age_frames > panel.max_age_frames[index]) {
                    metric.availability = MetricAvailability::Stale;
                    metric.value.reset();
                }
            }
        }
        return snapshot;
    }

    std::optional<InspectorPanelHandle> Find(std::string_view id) const {
        if (!ValidText(id)) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < m_panels.size(); ++index) {
            const auto& panel = m_panels[index];
            if (panel.generation != 0 && std::string_view(panel.snapshot.id.data()) == id) {
                return InspectorPanelHandle{this, index, panel.generation};
            }
        }
        return std::nullopt;
    }

    InspectorPanelList List() const {
        InspectorPanelList result;
        for (std::size_t index = 0; index < m_panels.size(); ++index) {
            const auto& panel = m_panels[index];
            if (panel.generation != 0) {
                result.panels[result.count++] = {this, index, panel.generation};
            }
        }
        return result;
    }

    bool Unregister(InspectorPanelHandle handle) {
        if (!Contains(handle)) {
            return false;
        }
        m_panels[handle.slot] = {};
        return true;
    }
    void Shutdown() {
        m_panels = {};
    }

private:
    struct Panel {
        InspectorPanelSnapshot snapshot{};
        std::array<InspectorMetricSample, kInspectorMetricLimit> samples{};
        std::array<std::uint64_t, kInspectorMetricLimit> max_age_frames{};
        std::uint64_t generation = 0;
    };
    static bool ValidText(std::string_view text) {
        return !text.empty() && text.size() <= kInspectorTextLimit &&
               text.find('\0') == std::string_view::npos;
    }
    static void CopyText(InspectorText& destination, std::string_view text) {
        std::copy(text.begin(), text.end(), destination.begin());
        destination[text.size()] = '\0';
    }
    bool Contains(InspectorPanelHandle handle) const {
        return handle.owner == this && handle.slot < m_panels.size() && handle.generation != 0 &&
               m_panels[handle.slot].generation == handle.generation;
    }
    std::array<Panel, kInspectorPanelLimit> m_panels{};
    std::uint64_t m_generation = 0;
};
} // namespace luminumbra::core
