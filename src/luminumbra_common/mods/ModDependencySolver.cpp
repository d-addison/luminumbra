#include "ModDependencySolver.h"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace luminumbra::mods {
namespace {

void FoldByte(ModSetIdentity& hash, std::uint8_t byte) {
    hash = (hash ^ byte) * 1099511628211ULL;
}

void FoldString(ModSetIdentity& hash, std::string_view text) {
    // uint64 little-endian byte length followed by bytes, with no terminator.
    const auto size = static_cast<std::uint64_t>(text.size());
    for (unsigned shift = 0; shift < 64; shift += 8) {
        FoldByte(hash, static_cast<std::uint8_t>(size >> shift));
    }
    for (const char byte : text) {
        FoldByte(hash, static_cast<std::uint8_t>(byte));
    }
}

std::string CyclePath(const std::vector<std::set<std::size_t>>& edges,
                      const std::vector<std::size_t>& indegree,
                      std::span<const ModPackage> packages) {
    // At most kMaxModCount nodes; iterative DFS avoids platform stack limits.
    std::vector<std::uint8_t> color(edges.size(), 0);
    std::vector<std::size_t> path;
    using Iterator = std::set<std::size_t>::const_iterator;
    std::vector<Iterator> next;
    for (std::size_t start = 0; start < edges.size(); ++start) {
        if (indegree[start] == 0 || color[start] != 0) {
            continue;
        }
        path.push_back(start);
        next.push_back(edges[start].begin());
        color[start] = 1;
        while (!path.empty()) {
            const auto node = path.back();
            if (next.back() == edges[node].end()) {
                color[node] = 2;
                path.pop_back();
                next.pop_back();
                continue;
            }
            const auto target = *next.back();
            ++next.back();
            if (indegree[target] == 0 || color[target] == 2) {
                continue;
            }
            if (color[target] == 1) {
                std::string result = "mod dependency cycle: ";
                for (auto item = std::find(path.begin(), path.end(), target); item != path.end();
                     ++item) {
                    result += packages[*item].manifest.id + " -> ";
                }
                return result + packages[target].manifest.id;
            }
            color[target] = 1;
            path.push_back(target);
            next.push_back(edges[target].begin());
        }
    }
    return "mod dependency cycle";
}

} // namespace

bool ModDependencySolver::Solve(std::span<const ModPackage> packages,
                                ModSolution& out,
                                std::string& error) {
    error.clear();
    if (packages.size() > kMaxModCount) {
        error = "mod set exceeds 1024 packages";
        return false;
    }
    std::map<std::string_view, std::size_t> ids;
    for (std::size_t index = 0; index < packages.size(); ++index) {
        const auto& manifest = packages[index].manifest;
        if (!manifest.Validate(error)) {
            return false;
        }
        if (!ids.emplace(manifest.id, index).second) {
            error = "duplicate mod id: " + manifest.id;
            return false;
        }
    }
    // Graph has <=1024 nodes and <=4*128 declared edges per node. Repeated
    // constraints share one edge so they cannot inflate indegrees.
    std::vector<std::set<std::size_t>> edges(packages.size());
    std::vector<std::size_t> indegree(packages.size(), 0);
    const auto add_edge = [&edges, &indegree](std::size_t source, std::size_t target) {
        if (edges[source].insert(target).second) {
            ++indegree[target];
        }
    };
    for (const auto& [id, index] : ids) {
        const auto& manifest = packages[index].manifest;
        for (const auto& dependency : manifest.dependencies) {
            const auto found = ids.find(dependency.id);
            if (found == ids.end()) {
                if (dependency.optional) {
                    continue;
                }
                error = std::string(id) + " requires missing mod " + dependency.id;
                return false;
            }
            const auto& installed = packages[found->second].manifest;
            if (!ModVersionMatches(installed.version, dependency.version)) {
                error = std::string(id) + " requires " + dependency.id + " " + dependency.version +
                        "; installed " + installed.version;
                return false;
            }
            add_edge(found->second, index);
        }
        for (const auto& conflict : manifest.incompatible_with) {
            const auto found = ids.find(conflict.id);
            if (found != ids.end() &&
                ModVersionMatches(packages[found->second].manifest.version, conflict.version)) {
                error =
                    std::string(id) + " incompatible with " + conflict.id + " " + conflict.version;
                return false;
            }
        }
        for (const auto& predecessor : manifest.load_after) {
            const auto found = ids.find(predecessor);
            if (found != ids.end()) {
                add_edge(found->second, index);
            }
        }
        for (const auto& successor : manifest.load_before) {
            const auto found = ids.find(successor);
            if (found != ids.end()) {
                add_edge(index, found->second);
            }
        }
    }
    std::set<std::string_view> ready;
    for (const auto& [id, index] : ids) {
        if (indegree[index] == 0) {
            ready.insert(id);
        }
    }
    ModSolution result;
    result.order.reserve(packages.size());
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        const auto index = ids.at(id);
        result.order.push_back(index);
        const auto& package = packages[index];
        FoldString(result.identity, id);
        FoldString(result.identity, package.manifest.version);
        for (const auto byte : package.content_hash) {
            FoldByte(result.identity, byte);
        }
        for (const auto target : edges[index]) {
            if (--indegree[target] == 0) {
                ready.insert(packages[target].manifest.id);
            }
        }
    }
    if (result.order.size() != packages.size()) {
        error = CyclePath(edges, indegree, packages);
        return false;
    }
    out = std::move(result);
    return true;
}

} // namespace luminumbra::mods
