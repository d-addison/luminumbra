#include "rendering/synthetic_mesh_fixture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace luminumbra::test {
namespace {

constexpr std::uint32_t kLmeshMagic = 0x48534d4cU; // 'LMSH' little-endian
constexpr std::uint32_t kLtexMagic = 0x5845544cU;  // 'LTEX' little-endian
constexpr std::uint16_t kLtexVersion = 1;
constexpr std::uint32_t kTextureResolution = 512;

struct LmeshHeader {
    std::uint32_t magic;
    std::uint32_t vertex_count;
    std::uint32_t index_count;
    float bounding_sphere[4];
};

struct LmeshVertex {
    float position[3];
    float normal[3];
    float uv[2];
};

static_assert(sizeof(LmeshHeader) == 28);
static_assert(sizeof(LmeshVertex) == 32);

struct CubeSpec {
    float center_y;
    float half_x;
    float half_y;
    float half_z;
};

struct Rgba {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

constexpr std::array<const char*, 3> kMeshPaths = {
    "data/models/trees/tree_small_02_trunk.lmesh",
    "data/models/trees/tree_small_02_branches.lmesh",
    "data/models/trees/tree_small_02_leaves.lmesh",
};

constexpr std::array<const char*, 6> kTexturePaths = {
    "data/textures/models/tree_trunk_albedo_512.ltex",
    "data/textures/models/tree_trunk_normal_512.ltex",
    "data/textures/models/tree_branch_albedo_512.ltex",
    "data/textures/models/tree_branch_normal_512.ltex",
    "data/textures/models/tree_leaves_albedo_512.ltex",
    "data/textures/models/tree_leaves_normal_512.ltex",
};

bool HasAllFoliageAssets(const std::filesystem::path& root) {
    std::error_code error;
    for (const auto& path : kMeshPaths) {
        if (!std::filesystem::is_regular_file(root / path, error)) {
            return false;
        }
        error.clear();
    }
    for (const auto& path : kTexturePaths) {
        if (!std::filesystem::is_regular_file(root / path, error)) {
            return false;
        }
        error.clear();
    }
    return true;
}

std::filesystem::path CreateUniqueRoot() {
    static std::atomic<std::uint64_t> sequence = 0;
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temp_root = std::filesystem::temp_directory_path();

    for (int attempt = 0; attempt < 128; ++attempt) {
        const std::filesystem::path candidate =
            temp_root / ("luminumbra-synthetic-foliage-" + std::to_string(timestamp) + "-" +
                         std::to_string(sequence.fetch_add(1)));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            throw std::runtime_error("cannot create synthetic foliage fixture: " + error.message());
        }
    }

    throw std::runtime_error("cannot allocate a unique synthetic foliage fixture directory");
}

void LinkOrCopy(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    if (std::filesystem::is_directory(source, error)) {
        error.clear();
        std::filesystem::create_directory_symlink(source, destination, error);
        if (!error) {
            return;
        }

        error.clear();
        std::filesystem::create_directories(destination, error);
        if (error) {
            throw std::runtime_error("cannot create fixture directory '" + destination.string() +
                                     "': " + error.message());
        }
        for (const auto& entry : std::filesystem::directory_iterator(source)) {
            LinkOrCopy(entry.path(), destination / entry.path().filename());
        }
        return;
    }

    error.clear();
    std::filesystem::create_symlink(source, destination, error);
    if (!error) {
        return;
    }
    error.clear();
    std::filesystem::create_hard_link(source, destination, error);
    if (!error) {
        return;
    }
    error.clear();
    std::filesystem::copy_file(source, destination, error);
    if (error) {
        throw std::runtime_error("cannot mirror runtime asset '" + source.string() +
                                 "': " + error.message());
    }
}

void MirrorEntries(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   const std::vector<std::string>& excluded_names = {}) {
    std::filesystem::create_directories(destination);
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        const std::string name = entry.path().filename().string();
        bool excluded = false;
        for (const std::string& excluded_name : excluded_names) {
            if (name == excluded_name) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            LinkOrCopy(entry.path(), destination / entry.path().filename());
        }
    }
}

void BuildRuntimeOverlay(const std::filesystem::path& source_root,
                         const std::filesystem::path& fixture_root) {
    LinkOrCopy(source_root / "res", fixture_root / "res");
    LinkOrCopy(source_root / "worlds", fixture_root / "worlds");

    const std::filesystem::path source_data = source_root / "data";
    const std::filesystem::path fixture_data = fixture_root / "data";
    MirrorEntries(source_data, fixture_data, {"models", "textures"});

    const std::filesystem::path source_models = source_data / "models";
    const std::filesystem::path fixture_models = fixture_data / "models";
    MirrorEntries(source_models, fixture_models, {"trees"});
    MirrorEntries(source_models / "trees", fixture_models / "trees");

    const std::filesystem::path source_textures = source_data / "textures";
    const std::filesystem::path fixture_textures = fixture_data / "textures";
    MirrorEntries(source_textures, fixture_textures, {"models"});
    const std::filesystem::path source_model_textures = source_textures / "models";
    const std::filesystem::path fixture_model_textures = fixture_textures / "models";
    std::error_code error;
    if (std::filesystem::is_directory(source_model_textures, error)) {
        MirrorEntries(source_model_textures, fixture_model_textures);
    } else {
        std::filesystem::create_directories(fixture_model_textures);
    }
}

std::vector<LmeshVertex> MakeCubeVertices(const CubeSpec& cube) {
    const float x0 = -cube.half_x;
    const float x1 = cube.half_x;
    const float y0 = cube.center_y - cube.half_y;
    const float y1 = cube.center_y + cube.half_y;
    const float z0 = -cube.half_z;
    const float z1 = cube.half_z;
    return {
        {{x1, y0, z0}, {1, 0, 0}, {0, 0}},  {{x1, y1, z0}, {1, 0, 0}, {0, 1}},
        {{x1, y1, z1}, {1, 0, 0}, {1, 1}},  {{x1, y0, z1}, {1, 0, 0}, {1, 0}},
        {{x0, y0, z1}, {-1, 0, 0}, {0, 0}}, {{x0, y1, z1}, {-1, 0, 0}, {0, 1}},
        {{x0, y1, z0}, {-1, 0, 0}, {1, 1}}, {{x0, y0, z0}, {-1, 0, 0}, {1, 0}},
        {{x0, y1, z0}, {0, 1, 0}, {0, 0}},  {{x0, y1, z1}, {0, 1, 0}, {0, 1}},
        {{x1, y1, z1}, {0, 1, 0}, {1, 1}},  {{x1, y1, z0}, {0, 1, 0}, {1, 0}},
        {{x0, y0, z1}, {0, -1, 0}, {0, 0}}, {{x0, y0, z0}, {0, -1, 0}, {0, 1}},
        {{x1, y0, z0}, {0, -1, 0}, {1, 1}}, {{x1, y0, z1}, {0, -1, 0}, {1, 0}},
        {{x0, y0, z1}, {0, 0, 1}, {0, 0}},  {{x1, y0, z1}, {0, 0, 1}, {1, 0}},
        {{x1, y1, z1}, {0, 0, 1}, {1, 1}},  {{x0, y1, z1}, {0, 0, 1}, {0, 1}},
        {{x1, y0, z0}, {0, 0, -1}, {0, 0}}, {{x0, y0, z0}, {0, 0, -1}, {1, 0}},
        {{x0, y1, z0}, {0, 0, -1}, {1, 1}}, {{x1, y1, z0}, {0, 0, -1}, {0, 1}},
    };
}

void WriteMesh(const std::filesystem::path& path, const CubeSpec& cube) {
    static constexpr std::array<std::uint32_t, 36> kIndices = {
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    };
    const std::vector<LmeshVertex> vertices = MakeCubeVertices(cube);
    const float radius = std::sqrt(cube.half_x * cube.half_x + cube.half_y * cube.half_y +
                                   cube.half_z * cube.half_z);
    const LmeshHeader header = {kLmeshMagic,
                                static_cast<std::uint32_t>(vertices.size()),
                                static_cast<std::uint32_t>(kIndices.size()),
                                {0.0f, cube.center_y, 0.0f, radius}};

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create synthetic mesh: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(vertices.data()),
                 static_cast<std::streamsize>(vertices.size() * sizeof(LmeshVertex)));
    output.write(reinterpret_cast<const char*>(kIndices.data()),
                 static_cast<std::streamsize>(kIndices.size() * sizeof(std::uint32_t)));
    if (!output) {
        throw std::runtime_error("cannot write synthetic mesh: " + path.string());
    }
}

void WriteTexture(const std::filesystem::path& path, const Rgba color) {
    constexpr std::uint16_t mip_count = 1;
    constexpr std::uint8_t channels = 4;
    const std::vector<Rgba> pixels(
        static_cast<std::size_t>(kTextureResolution) * kTextureResolution, color);

    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create synthetic texture: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(&kLtexMagic), sizeof(kLtexMagic));
    output.write(reinterpret_cast<const char*>(&kLtexVersion), sizeof(kLtexVersion));
    output.write(reinterpret_cast<const char*>(&mip_count), sizeof(mip_count));
    output.write(reinterpret_cast<const char*>(&kTextureResolution), sizeof(kTextureResolution));
    output.write(reinterpret_cast<const char*>(&kTextureResolution), sizeof(kTextureResolution));
    output.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    output.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size() * sizeof(Rgba)));
    if (!output) {
        throw std::runtime_error("cannot write synthetic texture: " + path.string());
    }
}

void GenerateMissingAssets(const std::filesystem::path& root) {
    const std::array<CubeSpec, 3> cubes = {
        CubeSpec{1.5f, 0.24f, 1.5f, 0.24f},
        CubeSpec{2.15f, 1.1f, 0.16f, 0.16f},
        CubeSpec{3.0f, 1.25f, 0.95f, 1.0f},
    };
    for (std::size_t index = 0; index < kMeshPaths.size(); ++index) {
        const std::filesystem::path path = root / kMeshPaths[index];
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            WriteMesh(path, cubes[index]);
        }
    }

    const std::array<Rgba, 6> colors = {
        Rgba{92, 58, 31, 255},
        Rgba{128, 128, 255, 255},
        Rgba{106, 68, 35, 255},
        Rgba{128, 128, 255, 255},
        Rgba{52, 126, 44, 255},
        Rgba{128, 128, 255, 255},
    };
    for (std::size_t index = 0; index < kTexturePaths.size(); ++index) {
        const std::filesystem::path path = root / kTexturePaths[index];
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            WriteTexture(path, colors[index]);
        }
    }
}

} // namespace

SyntheticMeshFixture::SyntheticMeshFixture(const std::filesystem::path& source_root) {
    if (HasAllFoliageAssets(source_root)) {
        return;
    }

    original_working_directory_ = std::filesystem::current_path();
    root_ = CreateUniqueRoot();
    try {
        BuildRuntimeOverlay(source_root, root_);
        GenerateMissingAssets(root_);
        std::filesystem::current_path(root_);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        root_.clear();
        original_working_directory_.clear();
        throw;
    }
}

SyntheticMeshFixture::~SyntheticMeshFixture() {
    if (root_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::current_path(original_working_directory_, error);
    error.clear();
    std::filesystem::remove_all(root_, error);
}

} // namespace luminumbra::test
