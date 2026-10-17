#pragma once

#include "GraphicsPresets.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace luminumbra::core {

inline constexpr std::uint32_t kGraphicsPresetRulesRevision = 1;
// Each driver identity field is limited to 1024 bytes before copying/normalizing.
// Overlong fields are refused rather than matched against a truncated model.
inline constexpr std::size_t kMaxGraphicsIdentityBytes = 1024;

enum class GraphicsIdentityRule {
    MissingIdentity,
    SoftwareOrGeneric,
    UnknownModel,
    AllowlistedModel
};

struct GraphicsHardwareInputs {
    std::string vendor;
    std::string renderer;
    std::string version;
    unsigned int logicalProcessorCount = 0;
    std::optional<std::uint64_t> dedicatedVideoMemoryBytes;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
};

struct GraphicsPresetReason {
    GraphicsIdentityRule identityRule = GraphicsIdentityRule::MissingIdentity;
    GraphicsPreset gpuCeiling = GraphicsPreset::Low;
    GraphicsPreset cpuCeiling = GraphicsPreset::Low;
    std::optional<GraphicsPreset> vramCeiling;
    // Every ceiling equal to the recommendation is binding, including ties.
    bool gpuBinding = true;
    bool cpuBinding = true;
    bool vramBinding = false;
    bool operator==(const GraphicsPresetReason&) const = default;
};

struct GraphicsPresetRecommendation {
    std::uint32_t rulesRevision = kGraphicsPresetRulesRevision;
    GraphicsPreset preset = GraphicsPreset::Low;
    std::string normalizedVendor;
    std::string normalizedRenderer;
    std::string normalizedVersion;
    std::string matchedModel;
    GraphicsPresetReason reason;
    bool operator==(const GraphicsPresetRecommendation&) const = default;
};

// Pure, locale-independent classification. All three identity fields are required.
// Only approved models with known vendor/driver decorations may exceed Low;
// unlisted mobile, OEM and other model variants always fall back to Low.
// Framebuffer dimensions are diagnostic inputs; revision 1 adds no resolution ceiling.
[[nodiscard]] GraphicsPresetRecommendation
ClassifyGraphicsPreset(const GraphicsHardwareInputs& inputs);

// NVX reports dedicated capacity in KB (1024 bytes), not current free memory.
// Missing advertisement, invalid values and conversion overflow leave capacity unknown.
[[nodiscard]] std::optional<std::uint64_t>
DedicatedVideoMemoryBytesFromNvx(bool extensionAdvertised, std::int64_t dedicatedMemoryKB);
[[nodiscard]] std::string_view GraphicsPresetDisplayName(GraphicsPreset preset);
[[nodiscard]] std::string_view GraphicsIdentityRuleName(GraphicsIdentityRule rule);

} // namespace luminumbra::core
