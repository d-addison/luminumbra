#pragma once

#include <filesystem>

namespace Luminumbra::TestSupport {

// Provision the shipped preset's worldgen dependencies without copying systems
// configuration or client assets into a test's isolated runtime root.
inline void CopyWorldgenTestData(const std::filesystem::path& source_root,
                                 const std::filesystem::path& runtime_root) {
    namespace fs = std::filesystem;
    const auto common = runtime_root / "data/common";
    fs::create_directories(common);
    fs::copy_file(source_root / "data/common/biomes.json", common / "biomes.json");
    fs::copy(
        source_root / "data/common/structures", common / "structures", fs::copy_options::recursive);
}

} // namespace Luminumbra::TestSupport
