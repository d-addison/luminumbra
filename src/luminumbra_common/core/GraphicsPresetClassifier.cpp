#include "GraphicsPresetClassifier.h"

#include <algorithm>
#include <array>
#include <limits>

namespace luminumbra::core {
namespace {

constexpr char AsciiUpper(char value) {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

std::string NormalizeIdentity(std::string_view text) {
    if (text.size() > kMaxGraphicsIdentityBytes) {
        return {};
    }
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        // Trademark annotations may appear inside a model name.
        if (text[i] == '(' && i + 2 < text.size() && AsciiUpper(text[i + 1]) == 'R' &&
            text[i + 2] == ')') {
            i += 2;
            continue;
        }
        if (text[i] == '(' && i + 3 < text.size() && AsciiUpper(text[i + 1]) == 'T' &&
            AsciiUpper(text[i + 2]) == 'M' && text[i + 3] == ')') {
            i += 3;
            continue;
        }
        const char value = AsciiUpper(text[i]);
        if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')) {
            normalized.push_back(value);
        } else if (!normalized.empty() && normalized.back() != ' ') {
            normalized.push_back(' ');
        }
    }
    if (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

bool HasTokens(std::string_view text, std::string_view tokens) {
    for (std::size_t pos = text.find(tokens); pos != std::string_view::npos;
         pos = text.find(tokens, pos + 1)) {
        const std::size_t end = pos + tokens.size();
        if ((pos == 0 || text[pos - 1] == ' ') && (end == text.size() || text[end] == ' ')) {
            return true;
        }
    }
    return false;
}

bool IsSoftwareOrGeneric(std::string_view text) {
    constexpr std::array markers{"LLVMPIPE",
                                 "SOFTPIPE",
                                 "SWIFTSHADER",
                                 "SOFTWARE",
                                 "GENERIC",
                                 "MICROSOFT BASIC RENDER",
                                 "MICROSOFT BASIC DISPLAY"};
    for (const std::string_view marker : markers) {
        if (HasTokens(text, marker)) {
            return true;
        }
    }
    return false;
}

GraphicsPreset CpuCeiling(unsigned int count) {
    if (count <= 4) {
        return GraphicsPreset::Low;
    }
    if (count <= 7) {
        return GraphicsPreset::Medium;
    }
    if (count <= 15) {
        return GraphicsPreset::High;
    }
    return GraphicsPreset::Ultra;
}

GraphicsPreset VramCeiling(std::uint64_t bytes) {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    if (bytes < 3 * gib) {
        return GraphicsPreset::Low;
    }
    if (bytes < 6 * gib) {
        return GraphicsPreset::Medium;
    }
    if (bytes < 8 * gib) {
        return GraphicsPreset::High;
    }
    return GraphicsPreset::Ultra;
}

struct ModelRule {
    std::string_view model;
    GraphicsPreset ceiling;
};

constexpr std::array kModelRules{
    ModelRule{"GTX 1060", GraphicsPreset::Low},
    ModelRule{"RX 580", GraphicsPreset::Low},
    ModelRule{"GTX 1660 SUPER", GraphicsPreset::Medium},
    ModelRule{"RTX 2060", GraphicsPreset::Medium},
    ModelRule{"RX 5600 XT", GraphicsPreset::Medium},
    ModelRule{"RTX 3060 TI", GraphicsPreset::High},
    ModelRule{"RTX 3070", GraphicsPreset::High},
    ModelRule{"RX 6700 XT", GraphicsPreset::High},
    ModelRule{"RTX 5070 TI", GraphicsPreset::Ultra},
};

bool ConsumeTokens(std::string_view& text, std::string_view tokens) {
    if (!text.starts_with(tokens) || (text.size() != tokens.size() && text[tokens.size()] != ' ')) {
        return false;
    }
    text.remove_prefix(tokens.size());
    if (!text.empty()) {
        text.remove_prefix(1);
    }
    return true;
}

bool IsDecimal(std::string_view token) {
    return !token.empty() && std::all_of(token.begin(), token.end(), [](char value) {
        return value >= '0' && value <= '9';
    });
}

bool StartsWithIgnoringCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char left, char right) {
               return AsciiUpper(left) == AsciiUpper(right);
           });
}

bool EqualsIgnoringCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() && StartsWithIgnoringCase(left, right);
}

std::string_view TrimWhitespace(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

bool IsMesaChipName(std::string_view token) {
    return (StartsWithIgnoringCase(token, "NAVI") && IsDecimal(token.substr(4))) ||
           (StartsWithIgnoringCase(token, "POLARIS") && IsDecimal(token.substr(7)));
}

bool ConsumeDecimal(std::string_view& text) {
    const auto end = text.find_first_not_of("0123456789");
    const auto digits = end == std::string_view::npos ? text.size() : end;
    text.remove_prefix(digits);
    return digits != 0;
}

bool ConsumeVersion(std::string_view& text, unsigned int components) {
    for (unsigned int component = 0; component < components; ++component) {
        if (component != 0) {
            if (text.empty() || text.front() != '.') {
                return false;
            }
            text.remove_prefix(1);
        }
        if (!ConsumeDecimal(text)) {
            return false;
        }
    }
    return true;
}

bool IsLabelledVersion(std::string_view text, std::string_view label, unsigned int components) {
    if (!StartsWithIgnoringCase(text, label) || text.size() == label.size()) {
        return false;
    }
    text.remove_prefix(label.size());
    const auto version = TrimWhitespace(text);
    if (version.size() == text.size()) {
        return false;
    }
    text = version;
    return ConsumeVersion(text, components) && text.empty();
}

bool IsKernelRelease(std::string_view text) {
    if (!ConsumeVersion(text, 2)) {
        return false;
    }
    if (text.size() >= 2 && text.front() == '.' && text[1] >= '0' && text[1] <= '9') {
        text.remove_prefix(1);
        (void)ConsumeDecimal(text);
    }
    if (text.empty()) {
        return true;
    }
    if (text.front() != '-' && text.front() != '+' && text.front() != '.') {
        return false;
    }
    // Distribution/local-version suffixes are one opaque release token, never model words.
    return text.size() > 1 && std::all_of(text.begin(), text.end(), [](char value) {
               const char upper = AsciiUpper(value);
               return (upper >= 'A' && upper <= 'Z') || (value >= '0' && value <= '9') ||
                      value == '-' || value == '+' || value == '.' || value == '_';
           });
}

bool IsMesaDecoration(std::string_view text) {
    // At most five fields: optional driver, chip, LLVM version, DRM version, kernel release.
    std::array<std::string_view, 5> fields{};
    std::size_t count = 0;
    while (true) {
        if (count == fields.size()) {
            return false;
        }
        const auto comma = text.find(',');
        const auto field = TrimWhitespace(text.substr(0, comma));
        if (field.empty()) {
            return false;
        }
        fields[count++] = field;
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (count == 1 && StartsWithIgnoringCase(fields[0], "RADV")) {
        const auto chipText = fields[0].substr(4);
        const auto chip = TrimWhitespace(chipText);
        return chip.size() != chipText.size() && IsMesaChipName(chip);
    }
    // Older identities may report just LLVM and DRM versions.
    if (count == 2 && IsLabelledVersion(fields[0], "LLVM", 3)) {
        return IsLabelledVersion(fields[1], "DRM", 2);
    }
    const std::size_t chip = EqualsIgnoringCase(fields[0], "RADEONSI") ? 1 : 0;
    return count == chip + 4 && IsMesaChipName(fields[chip]) &&
           IsLabelledVersion(fields[chip + 1], "LLVM", 3) &&
           IsLabelledVersion(fields[chip + 2], "DRM", 2) && IsKernelRelease(fields[chip + 3]);
}

struct RendererIdentity {
    std::string_view text;
    bool validDecoration;
};

RendererIdentity ParseRendererIdentity(std::string_view renderer) {
    for (auto open = renderer.find('('); open != std::string_view::npos;
         open = renderer.find('(', open + 1)) {
        auto decoration = TrimWhitespace(renderer.substr(open + 1));
        const auto first = decoration.substr(0, decoration.find_first_of(" ,)\t\r\n\f\v"));
        if (!StartsWithIgnoringCase(first, "NAVI") && !StartsWithIgnoringCase(first, "POLARIS") &&
            !EqualsIgnoringCase(first, "RADEONSI") && !EqualsIgnoringCase(first, "RADV") &&
            !EqualsIgnoringCase(first, "LLVM")) {
            continue;
        }
        // Preserve the boundary even for malformed metadata: software markers in a kernel
        // release must never become renderer identity, and invalid metadata cannot admit a model.
        const auto identity = renderer.substr(0, open);
        if (!decoration.ends_with(')')) {
            return {identity, false};
        }
        decoration.remove_suffix(1);
        return {identity, IsMesaDecoration(decoration)};
    }
    return {renderer, true};
}

bool IsPciDeviceId(std::string_view token) {
    return token.size() == 10 && token.starts_with("0X") &&
           std::all_of(token.begin() + 2, token.end(), [](char value) {
               return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
           });
}

bool IsDriverDecoration(std::string_view text) {
    auto first = text.substr(0, text.find(' '));
    // ANGLE may place a PCI device ID before its Direct3D shader-model metadata.
    if (IsPciDeviceId(first)) {
        (void)ConsumeTokens(text, first);
        first = text.substr(0, text.find(' '));
        if (first != "DIRECT3D11") {
            return false;
        }
    }
    if (first != "PCIE" && first != "DIRECT3D11") {
        return false;
    }
    // Recognize driver metadata positively so unknown variant tokens cannot hide in it.
    while (!text.empty()) {
        const auto token = text.substr(0, text.find(' '));
        const bool numberedDriver = token.starts_with("D3D") && IsDecimal(token.substr(3));
        if (token != "PCIE" && token != "SSE2" && token != "DIRECT3D11" && token != "VS" &&
            token != "PS" && !numberedDriver && !IsDecimal(token)) {
            return false;
        }
        (void)ConsumeTokens(text, token);
    }
    return true;
}

std::optional<ModelRule> MatchModels(std::string_view renderer) {
    std::optional<ModelRule> matched;
    while (!renderer.empty()) {
        // Only known vendor/wrapper decorations may precede an approved model.
        while (ConsumeTokens(renderer, "ANGLE") || ConsumeTokens(renderer, "NVIDIA") ||
               ConsumeTokens(renderer, "AMD") || ConsumeTokens(renderer, "ATI") ||
               ConsumeTokens(renderer, "TECHNOLOGIES") || ConsumeTokens(renderer, "INC") ||
               ConsumeTokens(renderer, "CORPORATION") || ConsumeTokens(renderer, "GEFORCE") ||
               ConsumeTokens(renderer, "RADEON")) {}
        bool found = false;
        for (const auto& rule : kModelRules) {
            if (!ConsumeTokens(renderer, rule.model)) {
                continue;
            }
            // Ambiguous multi-model identities take the lowest recognized tier.
            if (!matched || rule.ceiling < matched->ceiling) {
                matched = rule;
            }
            if (rule.model == "GTX 1060") {
                (void)ConsumeTokens(renderer, "6GB");
            } else if (rule.model == "RX 580") {
                (void)ConsumeTokens(renderer, "SERIES");
            }
            (void)ConsumeTokens(renderer, "GPU");
            found = true;
            break;
        }
        if (!found) {
            return std::nullopt;
        }
        if (IsDriverDecoration(renderer)) {
            return matched;
        }
        // Any unrecognized remainder invalidates the whole identity on the next pass.
        // Only exact approved models and the decorations above can raise the Low fallback.
    }
    return matched;
}

} // namespace

GraphicsPresetRecommendation ClassifyGraphicsPreset(const GraphicsHardwareInputs& inputs) {
    GraphicsPresetRecommendation result;
    result.normalizedVendor = NormalizeIdentity(inputs.vendor);
    result.normalizedRenderer = NormalizeIdentity(inputs.renderer);
    result.normalizedVersion = NormalizeIdentity(inputs.version);
    auto& reason = result.reason;
    if (result.normalizedVendor.empty() || result.normalizedRenderer.empty() ||
        result.normalizedVersion.empty()) {
        reason.identityRule = GraphicsIdentityRule::MissingIdentity;
    } else {
        const auto identity = ParseRendererIdentity(inputs.renderer);
        const auto normalizedIdentity = NormalizeIdentity(identity.text);
        if (IsSoftwareOrGeneric(result.normalizedVendor) ||
            IsSoftwareOrGeneric(normalizedIdentity)) {
            reason.identityRule = GraphicsIdentityRule::SoftwareOrGeneric;
        } else {
            reason.identityRule = GraphicsIdentityRule::UnknownModel;
            if (const auto matched =
                    identity.validDecoration ? MatchModels(normalizedIdentity) : std::nullopt) {
                reason.gpuCeiling = matched->ceiling;
                result.matchedModel = matched->model;
                reason.identityRule = GraphicsIdentityRule::AllowlistedModel;
            }
        }
    }
    reason.cpuCeiling = CpuCeiling(inputs.logicalProcessorCount);
    result.preset = std::min(reason.gpuCeiling, reason.cpuCeiling);
    if (inputs.dedicatedVideoMemoryBytes) {
        reason.vramCeiling = VramCeiling(*inputs.dedicatedVideoMemoryBytes);
        result.preset = std::min(result.preset, *reason.vramCeiling);
    }
    reason.gpuBinding = reason.gpuCeiling == result.preset;
    reason.cpuBinding = reason.cpuCeiling == result.preset;
    reason.vramBinding = reason.vramCeiling && *reason.vramCeiling == result.preset;
    return result;
}

std::optional<std::uint64_t> DedicatedVideoMemoryBytesFromNvx(bool extensionAdvertised,
                                                              std::int64_t dedicatedMemoryKB) {
    if (!extensionAdvertised || dedicatedMemoryKB <= 0) {
        return std::nullopt;
    }
    const auto kilobytes = static_cast<std::uint64_t>(dedicatedMemoryKB);
    if (kilobytes > std::numeric_limits<std::uint64_t>::max() / 1024) {
        return std::nullopt;
    }
    return kilobytes * 1024;
}

std::string_view GraphicsPresetDisplayName(GraphicsPreset preset) {
    switch (preset) {
        case GraphicsPreset::Low:
            return "Low";
        case GraphicsPreset::Medium:
            return "Medium";
        case GraphicsPreset::High:
            return "High";
        case GraphicsPreset::Ultra:
            return "Ultra";
    }
    return "Low";
}

std::string_view GraphicsIdentityRuleName(GraphicsIdentityRule rule) {
    switch (rule) {
        case GraphicsIdentityRule::MissingIdentity:
            return "missing_identity";
        case GraphicsIdentityRule::SoftwareOrGeneric:
            return "software_or_generic";
        case GraphicsIdentityRule::UnknownModel:
            return "unknown_model";
        case GraphicsIdentityRule::AllowlistedModel:
            return "allowlisted_model";
    }
    return "missing_identity";
}

} // namespace luminumbra::core
