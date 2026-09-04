#pragma once

#include <filesystem>

namespace luminumbra::test {

// Builds a temporary runtime-root overlay containing valid fallback tree
// meshes and textures. Existing real assets are linked into the overlay and
// are never replaced or modified.
class SyntheticMeshFixture {
public:
    explicit SyntheticMeshFixture(const std::filesystem::path& source_root);
    ~SyntheticMeshFixture();

    SyntheticMeshFixture(const SyntheticMeshFixture&) = delete;
    SyntheticMeshFixture& operator=(const SyntheticMeshFixture&) = delete;

    [[nodiscard]] bool active() const {
        return !root_.empty();
    }

    [[nodiscard]] const std::filesystem::path& root() const {
        return root_;
    }

private:
    std::filesystem::path original_working_directory_;
    std::filesystem::path root_;
};

} // namespace luminumbra::test
