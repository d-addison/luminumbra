#pragma once

#include "DiagnosticCapture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {

enum class DiagnosticProvenance : std::uint8_t {
    Unknown,
    Mesh,
    Sdf,
    Heightfield,
    Unsupported
};

struct DiagnosticSubmission {
    // Already resolved repository-relative generic UTF-8 spelling; no filesystem access.
    // Unsupported producers may use an empty path and always mark coverage incomplete.
    std::string_view mesh_path;
    std::string_view shader_variant;
    std::array<std::int32_t, 3> chunk{};
    std::uint32_t lod = 0; // Non-LOD geometry uses zero.
    DiagnosticOwnership ownership = DiagnosticOwnership::Live;
    DiagnosticPass pass = DiagnosticPass::GBuffer;
    DiagnosticProvenance provenance = DiagnosticProvenance::Unknown;
};

// Capture-local, render-thread registry. Clear only between completed captures;
// serialize before handing a snapshot to a worker. Never iterates its lookup map.
class DiagnosticDrawTable {
public:
    // Bounds include escaped JSON: 65,536 draws and meshes, 1,024 UTF-8 bytes per
    // input string, and 16 MiB for the complete schema-1 document. Append is
    // amortized O(1), with bounded string work; lookup is expected O(1).
    bool submit(const DiagnosticSubmission& submission, DiagnosticId& output) {
        constexpr std::size_t max_entries = 65536;
        const bool unsupported = submission.provenance == DiagnosticProvenance::Unsupported;
        if (draws_.size() == max_entries || submission.mesh_path.size() > 1024 ||
            submission.shader_variant.empty() || submission.shader_variant.size() > 1024 ||
            static_cast<unsigned int>(submission.provenance) >
                static_cast<unsigned int>(DiagnosticProvenance::Unsupported) ||
            (!unsupported && submission.mesh_path.empty()) ||
            (!submission.mesh_path.empty() && !valid_path(submission.mesh_path))) {
            return false;
        }
        DiagnosticId encoded{};
        if (!EncodeDiagnosticId({.draw = static_cast<std::uint32_t>(draws_.size() + 1),
                                 .mesh = 1,
                                 .lod = submission.lod,
                                 .ownership = submission.ownership,
                                 .pass = submission.pass},
                                encoded)) {
            return false;
        }
        // Prefix distinguishes different provenance for the same resolved asset.
        const std::string key = std::to_string(static_cast<unsigned int>(submission.provenance)) +
                                ":" + std::string(submission.mesh_path);
        const auto found = mesh_ids_.find(key);
        const bool new_mesh = found == mesh_ids_.end();
        if (new_mesh && meshes_.size() == max_entries) {
            return false;
        }
        encoded[1] = new_mesh ? static_cast<std::uint32_t>(meshes_.size() + 1) : found->second;
        std::string mesh_json;
        std::string draw_json;
        try {
            // dump also refuses malformed UTF-8, before changing registry state.
            if (new_mesh) {
                mesh_json = nlohmann::json{{"id", encoded[1]},
                                           {"path", submission.mesh_path},
                                           {"provenance", submission.provenance}}
                                .dump();
            }
            draw_json = nlohmann::json{{"id", encoded[0]},
                                       {"mesh", encoded[1]},
                                       {"chunk", submission.chunk},
                                       {"lod", submission.lod},
                                       {"ownership", submission.ownership},
                                       {"pass", submission.pass},
                                       {"shader_variant", submission.shader_variant},
                                       {"lod_convention", "zero_for_non_lod"},
                                       {"supported", !unsupported}}
                            .dump();
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        const std::size_t added = draw_json.size() + (draws_.empty() ? 0 : 1) + mesh_json.size() +
                                  (new_mesh && !meshes_.empty() ? 1 : 0);
        if (added > max_json_bytes - json_bytes_) {
            return false;
        }
        // Roll back only this append if an allocation fails; existing IDs survive.
        const auto old_meshes = meshes_.size();
        try {
            if (new_mesh) {
                mesh_ids_.emplace(key, encoded[1]);
                meshes_.push_back(std::move(mesh_json));
            }
            draws_.push_back({encoded, std::move(draw_json), unsupported});
        } catch (const std::exception&) {
            meshes_.resize(old_meshes);
            if (new_mesh) {
                mesh_ids_.erase(key);
            }
            return false;
        }
        json_bytes_ += added;
        unsupported_ = unsupported_ || unsupported;
        output = encoded;
        return true;
    }

    // Const O(1) join diagnostic. Background and unresolved IDs return nullptr.
    const DiagnosticId* resolve(std::uint32_t draw) const {
        return draw == 0 || draw > draws_.size() ? nullptr : &draws_[draw - 1].id;
    }
    bool complete_coverage() const {
        return !unsupported_;
    }
    // Capture-only validation, bounded by the 65,536-entry submission limit.
    bool complete_deferred_coverage() const {
        for (const auto& draw : draws_) {
            if (draw.unsupported &&
                (draw.id[3] >> 8U) == static_cast<std::uint32_t>(DiagnosticPass::GBuffer)) {
                return false;
            }
        }
        return true;
    }
    std::string json() const {
        std::string result;
        result.reserve(json_bytes_);
        result = prefix;
        append(result, meshes_);
        result += middle;
        for (std::size_t index = 0; index < draws_.size(); ++index) {
            if (index != 0) {
                result += ',';
            }
            result += draws_[index].json;
        }
        result += suffix;
        return result;
    }
    // Explicit submission records only; never infer coverage from pixel colors.
    // The subset cannot exceed the registry's existing 16 MiB JSON bound.
    void append_unsupported_json(std::string& result) const {
        result += '[';
        bool first = true;
        for (const auto& draw : draws_) {
            if (draw.unsupported) {
                if (!first) {
                    result += ',';
                }
                result += draw.json;
                first = false;
            }
        }
        result += ']';
    }
    void clear() {
        draws_.clear();
        meshes_.clear();
        mesh_ids_.clear();
        json_bytes_ = empty_json_bytes;
        unsupported_ = false;
    }

private:
    static constexpr std::string_view prefix = "{\"schema\":1,\"meshes\":[";
    static constexpr std::string_view middle = "],\"draws\":[";
    static constexpr std::string_view suffix = "]}";
    static constexpr std::size_t empty_json_bytes = prefix.size() + middle.size() + suffix.size();
    static constexpr std::size_t max_json_bytes = 16 * 1024 * 1024;
    struct Draw {
        DiagnosticId id;
        std::string json;
        bool unsupported = false;
    };
    static bool valid_path(std::string_view path) {
        if (path.front() == '/' || path.find_first_of("\\:") != std::string_view::npos) {
            return false;
        }
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            const auto component =
                path.substr(start, end == std::string_view::npos ? end : end - start);
            if (component.empty() || component == "." || component == "..") {
                return false;
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        for (const char character : path) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 32 || byte == 127) {
                return false;
            }
        }
        return true;
    }
    static void append(std::string& result, const std::vector<std::string>& entries) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index != 0) {
                result += ',';
            }
            result += entries[index];
        }
    }
    std::vector<Draw> draws_;
    std::vector<std::string> meshes_;
    std::unordered_map<std::string, std::uint32_t> mesh_ids_;
    std::size_t json_bytes_ = empty_json_bytes;
    bool unsupported_ = false;
};

} // namespace Luminumbra::Rendering
