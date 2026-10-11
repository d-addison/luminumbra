#include "SyntheticSaveFixture.h"

#include "WorldSaveService.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Luminumbra::Persistence {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::uint64_t SplitMix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Hash stream for chunk index i and lattice element k (unsigned wraparound is intended).
std::uint64_t LatticeHash(std::uint64_t seed, std::uint64_t i, std::uint64_t k) {
    return SplitMix64(seed ^ (i * 0x9E3779B97F4A7C15ull) ^ (k * 0xD6E8FEB86659FD93ull));
}

bool ValidWorldId(const std::string& id) {
    if (id.empty())
        return false;
    for (const char c : id) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed)
            return false;
    }
    return true;
}

SyntheticSaveReport Fail(SyntheticSaveReport report, std::string error) {
    report.ok = false;
    report.error = std::move(error);
    return report;
}

void FillChunk(Chunk& chunk, const SyntheticSaveSpec& spec, std::uint64_t index) {
    const std::size_t sx = static_cast<std::size_t>(CHUNK_SIZE_X) + 1;
    const std::size_t sy = static_cast<std::size_t>(CHUNK_SIZE_Y) + 1;
    const std::size_t sz = static_cast<std::size_t>(CHUNK_SIZE_Z) + 1;
    const std::size_t sdf_count = sx * sy * sz;
    const std::size_t heightmap_count = sx * sz;
    chunk.sdf_data.assign(sdf_count, 1.0f);
    chunk.heightmap_data.assign(heightmap_count, 8.0f);
    if (spec.payload == FixturePayload::Noise) {
        for (std::size_t k = 0; k < sdf_count; ++k) {
            const auto h = LatticeHash(spec.seed, index, k);
            chunk.sdf_data[k] = (static_cast<int>(h >> 56) - 128) / 16.0f;
        }
        for (std::size_t k = 0; k < heightmap_count; ++k) {
            const auto h = LatticeHash(spec.seed, index, k + 0x1000000ull);
            chunk.heightmap_data[k] = 8.0f + (static_cast<int>(h >> 56) % 32) / 4.0f;
        }
    }
    if (index < spec.edit_count) {
        for (std::size_t k = 0; k < sdf_count; ++k) {
            const int x = static_cast<int>(k % sx);
            const int y = static_cast<int>((k / sx) % sy);
            const int z = static_cast<int>(k / (sx * sy));
            int m = 8 - (std::abs(x - 8) + std::abs(y - 8) + std::abs(z - 8));
            if (m < 0)
                m = 0;
            chunk.sdf_data[k] -= static_cast<float>(m) / 4.0f;
        }
    }
}

} // namespace

// Spiral over columns: one column per pair of chunks, so chunk i uses column i / 2.
// Each column is computed once in a single O(n) walk.
std::vector<IVec3> SyntheticChunkCoords(std::uint32_t count) {
    constexpr int dx[4] = {1, 0, -1, 0};
    constexpr int dz[4] = {0, 1, 0, -1};
    const std::size_t columns = (static_cast<std::size_t>(count) + 1) / 2;
    std::vector<std::pair<int, int>> cols;
    cols.reserve(columns);
    if (columns > 0)
        cols.emplace_back(0, 0);
    int x = 0;
    int z = 0;
    int dir = 0;
    int run = 1;
    int leg = 0;
    while (cols.size() < columns) {
        for (int s = 0; s < run && cols.size() < columns; ++s) {
            x += dx[dir];
            z += dz[dir];
            cols.emplace_back(x, z);
        }
        dir = (dir + 1) % 4;
        if (++leg % 2 == 0)
            ++run;
    }
    std::vector<IVec3> coords;
    coords.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto [cx, cz] = cols[i / 2];
        coords.emplace_back(cx, static_cast<int>(i % 2), cz);
    }
    return coords;
}

SyntheticSaveReport WriteSyntheticSave(const SyntheticSaveSpec& spec, const fs::path& root) {
    SyntheticSaveReport report;
    report.save_dir = root / "worlds/saves" / spec.world_id;
    if (spec.chunk_count < 1)
        return Fail(std::move(report), "chunk_count must be at least 1.");
    if (spec.edit_count > spec.chunk_count)
        return Fail(std::move(report), "edit_count must not exceed chunk_count.");
    if (!ValidWorldId(spec.world_id))
        return Fail(std::move(report), "world_id must match [A-Za-z0-9_-]+.");

    std::error_code ec;
    const bool exists = fs::exists(report.save_dir, ec);
    if (ec)
        return Fail(std::move(report), "cannot inspect save directory: " + ec.message());
    if (exists)
        return Fail(std::move(report), "save already exists: " + report.save_dir.generic_string());
    fs::create_directories(report.save_dir, ec);
    if (ec)
        return Fail(std::move(report), "cannot create save directory: " + ec.message());

    const json metadata = {
        {"container_version", WorldSaveService::kContainerVersion},
        {"name", spec.world_name},
        {"seed", std::to_string(spec.seed)},
        {"worldType", "default"},
        {"creationTime", 1},
        {"spawnPoint", {{"x", 8}, {"y", 18}, {"z", 8}}},
        {"waterSimCursor", 0},
    };
    std::vector<std::string> errors;
    if (!WorldSaveService::save_metadata(metadata.dump(4) + "\n", report.save_dir, &errors))
        return Fail(std::move(report), errors.empty() ? "metadata write failed" : errors.front());

    if (!spec.preset_source.empty()) {
        if (!fs::copy_file(spec.preset_source, report.save_dir / "preset.json", ec))
            return Fail(std::move(report),
                        "cannot copy preset: " + spec.preset_source.generic_string() + " (" +
                            ec.message() + ")");
    }

    WorldStreamingState state;
    const auto coords = SyntheticChunkCoords(spec.chunk_count);
    for (std::size_t i = 0; i < coords.size(); ++i) {
        auto chunk = state.get_or_create_chunk(coords[i]);
        FillChunk(*chunk, spec, static_cast<std::uint64_t>(i));
        chunk->mark_sdf_loaded_or_edited();
    }
    std::vector<std::string> save_errors;
    if (!WorldSaveService{}.save_world(state, report.save_dir, &save_errors))
        return Fail(std::move(report),
                    save_errors.empty() ? "world save failed" : save_errors.front());
    report.chunks_written = spec.chunk_count;

    const auto region_dir = WorldSaveService::region_directory(report.save_dir);
    for (const auto& entry : fs::directory_iterator(region_dir, ec)) {
        const auto name = entry.path().filename().string();
        if (!name.ends_with(".lmr") || !entry.is_regular_file(ec))
            continue;
        ++report.region_files;
        report.region_bytes += entry.file_size(ec);
    }
    report.ok = true;
    return report;
}

} // namespace Luminumbra::Persistence
