#pragma once

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

inline bool ReadWorldgenFile(const std::filesystem::path& path, std::string& bytes) {
    bytes.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
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

} // namespace Luminumbra::World
