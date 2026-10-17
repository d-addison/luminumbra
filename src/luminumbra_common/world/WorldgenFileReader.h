#pragma once

#include "../core/BoundedFileReader.h"
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace Luminumbra::World {

// The preset manifest supplies a reader to observe the exact bytes consumed by
// the generator's loaders, including reads that lead to a rejected table/pool.
using WorldgenFileReader = std::function<bool(const std::filesystem::path&, std::string&)>;

// Keep status failures reproducible independently of native runtime error mappings.
using WorldgenFileStatusReader =
    std::function<std::filesystem::file_status(const std::filesystem::path&, std::error_code&)>;

inline std::filesystem::file_status ReadWorldgenSymlinkStatus(const std::filesystem::path& path,
                                                              std::error_code& ec) {
    return std::filesystem::symlink_status(path, ec);
}

// Presets and each referenced worldgen dependency are at most 16 MiB on disk.
inline constexpr std::size_t kMaxWorldgenFileBytes = std::size_t{16} * 1024u * 1024u;

inline bool ReadWorldgenFileWithStatus(const std::filesystem::path& path,
                                       std::string& bytes,
                                       const WorldgenFileStatusReader& read_status) {
    bytes.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(read_status(path, ec)) || ec)
        return false;
    try {
        return Filesystem::ReadBoundedFile(path, bytes, kMaxWorldgenFileBytes);
    } catch (const std::exception&) {
        bytes.clear();
        return false;
    }
}

inline bool ReadWorldgenFile(const std::filesystem::path& path, std::string& bytes) {
    return ReadWorldgenFileWithStatus(
        path, bytes, [](const std::filesystem::path& file, std::error_code& ec) {
            return std::filesystem::status(file, ec);
        });
}

} // namespace Luminumbra::World
