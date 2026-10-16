#pragma once

#include "luminumbra_common/core/CaptureContentPaths.h"
#include <array>
#include <filesystem>
#include <fstream>
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

inline bool ReadWorldgenFileWithStatus(const std::filesystem::path& path,
                                       std::string& bytes,
                                       const WorldgenFileStatusReader& read_status) {
    bytes.clear();
    if (!Filesystem::capture_content_paths.allows(path))
        return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(read_status(path, ec)) || ec)
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    std::array<char, 8192> buffer;
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           file.gcount() > 0) {
        bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
    }
    return !file.bad() && file.eof();
}

inline bool ReadWorldgenFile(const std::filesystem::path& path, std::string& bytes) {
    return ReadWorldgenFileWithStatus(
        path, bytes, [](const std::filesystem::path& file, std::error_code& ec) {
            return std::filesystem::status(file, ec);
        });
}

} // namespace Luminumbra::World
