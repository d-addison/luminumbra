#include "ContentVfs.h"

#include "../core/FilesystemPath.h"
#include "../world/WorldgenDependencySnapshot.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32) && !defined(__CYGWIN__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Luminumbra::Vfs {
namespace {
namespace fs = std::filesystem;

fs::file_status CheckComponent(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
        throw std::runtime_error("unsupported content path component");
    }
#if defined(_WIN32) && !defined(__CYGWIN__)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error("redirected or unreadable content path component");
    }
#endif
    return status;
}

fs::path CheckPath(const fs::path& root, std::string_view relative, bool directory) {
    const auto suffix = Filesystem::PathFromUtf8(relative);
    if (!relative.empty() && (root.native().size() >= 32767 ||
                              suffix.native().size() > 32767 - root.native().size() - 1)) {
        throw std::invalid_argument("content physical path exceeds bounds");
    }
    auto path = root;
    if (!fs::is_directory(CheckComponent(path))) {
        throw std::runtime_error("content mount root is not a directory");
    }
    // The validated logical byte bound caps component count and traversal work.
    for (auto component = suffix.begin(); component != suffix.end(); ++component) {
        path /= *component;
        const auto status = CheckComponent(path);
        auto next = component;
        ++next;
        const bool want_directory = next != suffix.end() || directory;
        if (fs::is_directory(status) != want_directory) {
            throw std::runtime_error("content path has the wrong type");
        }
    }
    return path;
}

std::unique_ptr<Filesystem::VerifiedFileInput>
OpenChecked(const fs::path& root, std::string_view relative, bool directory) {
    const auto path = CheckPath(root, relative, directory);
    // Expand 8.3 spellings without resolving links. Compare the owning reader's
    // resolution, so an intermediate redirect cannot authorize an outside read.
    std::error_code error;
    const auto expected = Filesystem::LongNamePath(path, error);
    if (error) {
        throw fs::filesystem_error("expand content path", path, error);
    }
    auto input = std::make_unique<Filesystem::VerifiedFileInput>(
        path,
        directory ? Filesystem::VerifiedFileInput::Kind::Directory
                  : Filesystem::VerifiedFileInput::Kind::File);
    if (!Filesystem::PathSpellingEqual(expected, input->resolved_path())) {
        throw std::runtime_error("content path changed resolution");
    }
    return input;
}

bool IsSimulationPath(std::string_view path) {
    const auto first = path.substr(0, path.find('/'));
    return first.size() == 3 && (first[0] == 's' || first[0] == 'S') &&
           (first[1] == 'i' || first[1] == 'I') && (first[2] == 'm' || first[2] == 'M');
}

void CheckResidency(ContentResidency residency) {
    switch (residency) {
        case ContentResidency::ClientCosmetic:
        case ContentResidency::SimData:
        case ContentResidency::SimScript:
        case ContentResidency::Native:
            return;
    }
    throw std::invalid_argument("unknown content residency");
}
} // namespace

bool Utf8ByteLess::operator()(std::string_view left, std::string_view right) const noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](char lhs, char rhs) {
            return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
        });
}

ContentVfs::ContentVfs()
    : ContentVfs(Limits{}) {}

ContentVfs::ContentVfs(const Limits& limits)
    : limits_(limits) {
    if (limits_.max_mounts == 0 || limits_.max_entries == 0 || limits_.max_path_bytes == 0 ||
        limits_.max_path_bytes > 32767 || limits_.max_path_storage_bytes == 0) {
        throw std::invalid_argument("invalid content inventory limits");
    }
}

void ContentVfs::ValidatePath(std::string_view path) const {
    if (path.empty() || path.size() > limits_.max_path_bytes || path.find('\\') != path.npos ||
        path.find(':') != path.npos || path.find('\0') != path.npos ||
        !World::IsValidDependencyPathUtf8(path)) {
        throw std::invalid_argument("invalid content logical path");
    }
    // Parse before filesystem conversion: empty components must not collapse.
    std::size_t offset = 0;
    while (offset <= path.size()) {
        const auto end = path.find('/', offset);
        const auto component =
            path.substr(offset, end == path.npos ? path.size() - offset : end - offset);
        // Trailing dots/spaces are Win32 aliases, including aliases of sim/.
        if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
            component.back() == ' ') {
            throw std::invalid_argument("invalid content logical component");
        }
        if (end == path.npos) {
            break;
        }
        offset = end + 1;
    }
}

ContentVfs::Index ContentVfs::Scan(const Mount& mount) const {
    const auto opened_root = OpenChecked(mount.root, {}, true);
    Index entries;
    std::size_t path_bytes = 0;
    // No directory links are followed. Both entry count and stored path bytes
    // are bounded before insertion; logical length also bounds nesting depth.
    for (const auto& entry : fs::recursive_directory_iterator(mount.root)) {
        if (entries.size() == limits_.max_entries) {
            throw std::runtime_error("content inventory entry limit exceeded");
        }
        const auto relative =
            Filesystem::GenericUtf8PathString(entry.path().lexically_relative(mount.root));
        ValidatePath(relative);
        if (relative.size() > limits_.max_path_storage_bytes - path_bytes) {
            throw std::runtime_error("content inventory path storage limit exceeded");
        }
        if (mount.residency == ContentResidency::ClientCosmetic && IsSimulationPath(relative)) {
            throw std::runtime_error("cosmetic content cannot provide simulation paths");
        }
        const bool directory = fs::is_directory(CheckComponent(entry.path()));
        const auto opened = OpenChecked(mount.root, relative, directory);
        entries.emplace(relative, Provider{mounts_.size(), directory});
        path_bytes += relative.size();
    }
    return entries;
}

void ContentVfs::AddMount(const fs::path& root, ContentResidency residency) {
    CheckResidency(residency);
    if (root.empty() || root.native().size() > 32767 || mounts_.size() == limits_.max_mounts) {
        throw std::invalid_argument("invalid content mount root or mount limit exceeded");
    }
    Mount mount{Filesystem::LexicallyNormalPath(Filesystem::AbsolutePath(root)), residency};
    if (mount.root.filename().empty() && mount.root != mount.root.root_path()) {
        mount.root = mount.root.parent_path();
    }
    if (mount.root.native().size() > 32767) {
        throw std::invalid_argument("content mount root exceeds bounds");
    }
    const auto entries = Scan(mount);
    auto merged = index_;
    std::size_t path_bytes = 0;
    for (const auto& [path, provider] : merged) {
        path_bytes += path.size(); // The published index already fits its budget.
    }
    // At most two bounded inventories are merged; directory entries precede
    // their descendants in byte order. A new file removes the old subtree.
    for (const auto& [path, provider] : entries) {
        if (!provider.directory) {
            const auto prefix = path + '/';
            auto descendant = merged.lower_bound(prefix);
            while (descendant != merged.end() && descendant->first.starts_with(prefix)) {
                path_bytes -= descendant->first.size();
                descendant = merged.erase(descendant);
            }
        }
        if (!merged.contains(path)) {
            if (merged.size() == limits_.max_entries ||
                path.size() > limits_.max_path_storage_bytes - path_bytes) {
                throw std::runtime_error("merged content inventory limit exceeded");
            }
            path_bytes += path.size();
        }
        merged.insert_or_assign(path, provider);
    }
    // All validation and allocation precedes publication. swap cannot throw.
    mounts_.push_back(std::move(mount));
    index_.swap(merged);
}

std::unique_ptr<Filesystem::VerifiedFileInput> ContentVfs::Open(std::string_view path) const {
    ValidatePath(path);
    const auto found = index_.find(path);
    if (found == index_.end()) {
        return nullptr;
    }
    if (found->second.directory) {
        throw std::runtime_error("content logical path is a directory");
    }
    const auto& mount = mounts_[found->second.mount_index];
    return OpenChecked(mount.root, path, false);
}

std::vector<ContentVfs::Entry> ContentVfs::ListChildren(std::string_view prefix) const {
    std::vector<Entry> children;
    // Scan only this directory's bounded inventory range, in UTF-8 byte order.
    for (auto entry = index_.lower_bound(prefix);
         entry != index_.end() && entry->first.starts_with(prefix);
         ++entry) {
        const auto name = std::string_view(entry->first).substr(prefix.size());
        if (name.find('/') == name.npos) {
            const auto& provider = entry->second;
            children.push_back(Entry{std::string(name),
                                     provider.directory,
                                     mounts_[provider.mount_index].residency,
                                     provider.mount_index});
        }
    }
    return children;
}

std::vector<ContentVfs::Entry> ContentVfs::ListDirectory() const {
    return ListChildren({});
}

std::vector<ContentVfs::Entry> ContentVfs::ListDirectory(std::string_view path) const {
    ValidatePath(path);
    const auto found = index_.find(path);
    if (found == index_.end()) {
        return {};
    }
    if (!found->second.directory) {
        throw std::runtime_error("content logical path is not a directory");
    }
    return ListChildren(std::string(path) + '/');
}

std::size_t ContentVfs::MountCount() const noexcept {
    return mounts_.size();
}

} // namespace Luminumbra::Vfs
