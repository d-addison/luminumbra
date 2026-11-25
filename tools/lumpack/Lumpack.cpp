#include "Lumpack.h"

#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/mods/ModDependencySolver.h"
#include "luminumbra_common/vfs/ContentVfs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Luminumbra::Lumpack {
namespace {
namespace mods = luminumbra::mods;
using nlohmann::json;
using Vfs::ContentResidency;
using Vfs::ContentVfs;

struct Diagnostic {
    std::string path;
    std::string code;
    std::string message;
};

void AddDiagnostic(std::vector<Diagnostic>& diagnostics,
                   const std::string& path,
                   std::string_view code,
                   std::string_view message) {
    // At most one diagnostic per package, plus one solver diagnostic. Bound
    // parser/solver text as well as the engine's existing manifest/path limits.
    diagnostics.push_back({path, std::string(code), std::string(message.substr(0, 1024))});
}

bool ReadPackage(const std::filesystem::path& root,
                 std::vector<Diagnostic>& diagnostics,
                 mods::ModPackage& package) {
    std::string path;
    std::string_view stage = "content";
    try {
        if (root.native().size() > 32767) {
            AddDiagnostic(diagnostics, {}, "content", "content directory path exceeds bounds");
            return false;
        }
        path = Filesystem::GenericUtf8PathString(root);
        ContentVfs vfs;
        // Discover the manifest through the same guarded inventory as content.
        // Residency is checked after parsing, before accepting this package.
        vfs.AddMount(root, ContentResidency::SimData);
        stage = "manifest_read";
        auto input = vfs.Open("mod.json");
        if (!input) {
            AddDiagnostic(diagnostics, path, "manifest_missing", "mod.json is required");
            return false;
        }
        if (input->size() > mods::kMaxManifestBytes) {
            AddDiagnostic(diagnostics, path, "manifest_size", "mod.json exceeds 65536 bytes");
            return false;
        }
        std::string bytes;
        std::array<char, 65536> buffer{}; // VerifiedFileInput's fixed 64 KiB read buffer.
        // Partial reads and growth are bounded by kMaxManifestBytes, including
        // work before parsing. Size and bytes come from this same owning reader.
        for (;;) {
            const auto count = input->read(buffer);
            if (count == 0) {
                break;
            }
            if (count > mods::kMaxManifestBytes - bytes.size()) {
                AddDiagnostic(diagnostics, path, "manifest_size", "mod.json exceeds 65536 bytes");
                return false;
            }
            bytes.append(buffer.data(), count);
        }
        if (bytes.size() != input->size()) {
            AddDiagnostic(
                diagnostics, path, "manifest_read", "mod.json size changed while reading");
            return false;
        }
        std::string error;
        if (!mods::ModManifest::Parse(bytes, package.manifest, error)) {
            AddDiagnostic(diagnostics, path, "manifest", error);
            return false;
        }
        if (package.manifest.tier == mods::ModTier::ClientCosmetic) {
            stage = "content";
            ContentVfs cosmetic;
            cosmetic.AddMount(root, ContentResidency::ClientCosmetic);
        }
        return true;
    } catch (const std::exception&) {
        // Filesystem exception text and the first rejected inventory entry can
        // vary by platform/enumeration order. Publish a stable stage diagnostic.
        AddDiagnostic(diagnostics, path, stage, "content directory or manifest read refused");
        return false;
    }
}

int Emit(std::vector<Diagnostic>& diagnostics,
         const std::vector<std::string>& order,
         int status,
         std::ostream& output) {
    const Vfs::Utf8ByteLess less;
    std::sort(diagnostics.begin(), diagnostics.end(), [&less](const auto& left, const auto& right) {
        if (left.path != right.path) {
            return less(left.path, right.path);
        }
        if (left.code != right.code) {
            return less(left.code, right.code);
        }
        return less(left.message, right.message);
    });
    auto messages = json::array();
    for (const auto& diagnostic : diagnostics) {
        messages.push_back({{"code", diagnostic.code},
                            {"message", diagnostic.message},
                            {"path", diagnostic.path}});
    }
    const json result{{"diagnostics", messages}, {"load_order", order}, {"valid", status == 0}};
    // Escape non-ASCII independently of the console's active code page.
    output << result.dump(-1, ' ', true, json::error_handler_t::replace) << '\n';
    output.flush();
    return output ? status : 1;
}
} // namespace

int Run(std::span<const std::filesystem::path> arguments, std::ostream& output) {
    try {
        std::vector<Diagnostic> diagnostics;
        std::vector<std::string> order;
        if (arguments.size() < 2 || arguments.size() - 1 > mods::kMaxModCount ||
            arguments.front() != "validate") {
            AddDiagnostic(diagnostics,
                          {},
                          "usage",
                          "lumpack validate <mod-dir> [<mod-dir> ...] (at most 1024 mods)");
            return Emit(diagnostics, order, 2, output);
        }
        // A separate VFS per package keeps the solver's 1024-package bound
        // independent of overlay mount limits. No overlays can hide bad input.
        std::vector<mods::ModPackage> packages;
        for (const auto& root : arguments.subspan(1)) {
            mods::ModPackage package;
            if (ReadPackage(root, diagnostics, package)) {
                packages.push_back(std::move(package));
            }
        }
        if (diagnostics.empty()) {
            // Stable indices also make the solver's cycle path independent of
            // command-line order. Only order is consumed, never its identity.
            std::sort(packages.begin(), packages.end(), [](const auto& left, const auto& right) {
                return left.manifest.id < right.manifest.id;
            });
            mods::ModSolution solution;
            std::string error;
            if (!mods::ModDependencySolver::Solve(packages, solution, error)) {
                AddDiagnostic(diagnostics, {}, "dependencies", error);
            } else {
                for (const auto index : solution.order) {
                    order.push_back(packages[index].manifest.id);
                }
            }
        }
        return Emit(diagnostics, order, diagnostics.empty() ? 0 : 1, output);
    } catch (const std::exception&) {
        return 1;
    }
}

} // namespace Luminumbra::Lumpack
