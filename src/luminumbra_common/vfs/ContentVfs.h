#pragma once

#include "../core/CaptureContentPaths.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Vfs {

enum class ContentResidency : std::uint8_t {
    ClientCosmetic,
    SimData,
    SimScript,
    Native
};

// Byte comparison is independent of locale and whether plain char is signed.
struct Utf8ByteLess {
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const noexcept;
};

// Read-only unpacked mounts. Add the base first, then mods in solver order.
// Inventories are snapshots: change packages only by constructing a new VFS.
// Later mounts replace exact names; files hide earlier directory subtrees.
// Invalid paths, redirects, wrong types and I/O errors throw; only an absent
// logical name returns nullptr (Open) or an empty list (ListDirectory).
// Mount and lookup operations must not run concurrently with AddMount.
class ContentVfs {
public:
    struct Limits {
        std::size_t max_mounts = 256;
        std::size_t max_entries = 65536;
        std::size_t max_path_bytes = 4096;
        std::size_t max_path_storage_bytes = 16 * 1024 * 1024;
    };
    struct Entry {
        std::string name;
        bool directory = false;
        ContentResidency residency = ContentResidency::SimData;
        std::size_t mount_index = 0;
        bool operator==(const Entry&) const = default;
    };

    ContentVfs();
    explicit ContentVfs(const Limits& limits);
    // Validates the entire mount before publishing anything. Cosmetic mounts
    // refuse sim/ (including case aliases), even an empty simulation directory.
    void AddMount(const std::filesystem::path& root, ContentResidency residency);
    [[nodiscard]] std::unique_ptr<Filesystem::VerifiedFileInput> Open(std::string_view path) const;
    [[nodiscard]] std::vector<Entry> ListDirectory() const;
    [[nodiscard]] std::vector<Entry> ListDirectory(std::string_view path) const;

    // Immutable diagnostic: successful mounts only, in overlay order.
    [[nodiscard]] std::size_t MountCount() const noexcept;

private:
    struct Mount {
        std::filesystem::path root;
        ContentResidency residency;
    };
    struct Provider {
        std::size_t mount_index;
        bool directory;
    };
    using Index = std::map<std::string, Provider, Utf8ByteLess>;
    void ValidatePath(std::string_view path) const;
    Index Scan(const Mount& mount) const;
    std::vector<Entry> ListChildren(std::string_view prefix) const;

    Limits limits_;
    std::vector<Mount> mounts_;
    Index index_;
};

} // namespace Luminumbra::Vfs
