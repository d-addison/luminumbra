#include "StructurePlacement.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "nlohmann/json.hpp"

#include "../core/Log.h"

namespace Luminumbra::World {
namespace {

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

void FnvMix(u64& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(bytes[i]);
        hash *= kFnvPrime;
    }
}

template<typename T>
void FnvMixValue(u64& hash, const T& value) {
    FnvMix(hash, &value, sizeof(T));
}

// SplitMix64: a tiny, exactly reproducible PRNG. Used for every placement and
// assembly choice so the result is a pure function of its seed (no libm, no
// container-order or wall-clock dependence - matches the sim determinism rules).
struct SplitMix64 {
    u64 state;
    explicit SplitMix64(u64 seed)
        : state(seed) {}
    u64 next() {
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform float in [0, 1).
    float next_unit() {
        return static_cast<float>(next() >> 11) * (1.0f / 9007199254740992.0f);
    }
    // Uniform int in [0, n) for n > 0.
    int next_int(int n) {
        return n <= 1 ? 0 : static_cast<int>(next() % static_cast<u64>(n));
    }
};

// Per-cell stream seed: mixes the world seed, the type salt, and the cell
// coordinates so different types and cells never share a stream.
u64 CellSeed(int world_seed, u32 salt, int cell_x, int cell_z) {
    u64 h = kFnvOffsetBasis;
    FnvMixValue(h, world_seed);
    FnvMixValue(h, salt);
    FnvMixValue(h, cell_x);
    FnvMixValue(h, cell_z);
    return h;
}

int FloorDiv(int value, int divisor) {
    const int q = value / divisor;
    const int r = value % divisor;
    return (r != 0 && ((r < 0) != (divisor < 0))) ? q - 1 : q;
}

void WarnUnknownKeys(const nlohmann::json& object,
                     std::initializer_list<const char*> known,
                     const std::string& context,
                     std::vector<std::string>& warnings) {
    if (!object.is_object()) {
        return;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool recognized = false;
        for (const char* key : known) {
            if (it.key() == key) {
                recognized = true;
                break;
            }
        }
        if (!recognized) {
            warnings.push_back("unknown key '" + it.key() + "' in " + context);
        }
    }
}

IVec3 ParseIVec3(const nlohmann::json& arr, const IVec3& fallback) {
    if (!arr.is_array() || arr.size() != 3) {
        return fallback;
    }
    return IVec3(arr[0].get<int>(), arr[1].get<int>(), arr[2].get<int>());
}

} // namespace

StructureTemplatePool LoadStructureTemplatePool(const std::filesystem::path& type_dir,
                                                const std::string& type) {
    StructureTemplatePool pool;
    pool.type = type;

    std::error_code ec;
    if (!std::filesystem::is_directory(type_dir, ec)) {
        pool.errors.push_back("structure type dir not found: " + type_dir.string());
        return pool;
    }

    // placement.json (optional; defaults documented in the header).
    const std::filesystem::path placement_path = type_dir / "placement.json";
    if (std::filesystem::exists(placement_path, ec)) {
        std::ifstream file(placement_path);
        nlohmann::json data;
        try {
            data = nlohmann::json::parse(file);
        } catch (const nlohmann::json::parse_error& e) {
            pool.errors.push_back(std::string("placement.json parse error: ") + e.what());
            return pool;
        }
        WarnUnknownKeys(data,
                        {"spacing", "separation", "salt", "density", "_comment"},
                        "placement.json",
                        pool.warnings);
        pool.spacing = std::max(1, data.value("spacing", pool.spacing));
        pool.separation = std::max(0, data.value("separation", pool.separation));
        pool.salt = data.value("salt", pool.salt);
        pool.density = std::clamp(data.value("density", pool.density), 0.0f, 1.0f);
    }

    // Every *.json under the type dir EXCEPT placement.json is a piece. Sort by
    // filename so piece order is deterministic regardless of directory order.
    std::vector<std::filesystem::path> piece_files;
    for (const auto& entry : std::filesystem::directory_iterator(type_dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& p = entry.path();
        if (p.extension() != ".json" || p.filename() == "placement.json") {
            continue;
        }
        piece_files.push_back(p);
    }
    std::sort(piece_files.begin(), piece_files.end());

    for (const auto& piece_path : piece_files) {
        std::ifstream file(piece_path);
        nlohmann::json data;
        try {
            data = nlohmann::json::parse(file);
        } catch (const nlohmann::json::parse_error& e) {
            pool.errors.push_back(piece_path.filename().string() + std::string(" parse error: ") +
                                  e.what());
            continue;
        }
        WarnUnknownKeys(data,
                        {"name", "boxes", "sockets", "_comment"},
                        piece_path.filename().string(),
                        pool.warnings);

        StructurePiece piece;
        piece.name = data.value("name", piece_path.stem().string());
        if (data.contains("boxes") && data["boxes"].is_array()) {
            for (const auto& box_json : data["boxes"]) {
                StructureBox box;
                box.min =
                    ParseIVec3(box_json.value("min", nlohmann::json::array()), IVec3(0, 0, 0));
                box.size =
                    ParseIVec3(box_json.value("size", nlohmann::json::array()), IVec3(1, 1, 1));
                box.size = IVec3(
                    std::max(1, box.size.x), std::max(1, box.size.y), std::max(1, box.size.z));
                box.material = static_cast<u8>(box_json.value("material", 0));
                piece.boxes.push_back(box);
            }
        }
        if (data.contains("sockets") && data["sockets"].is_array()) {
            for (const auto& socket_json : data["sockets"]) {
                StructureSocket socket;
                socket.name = socket_json.value("name", std::string());
                socket.position = ParseIVec3(socket_json.value("position", nlohmann::json::array()),
                                             IVec3(0, 0, 0));
                piece.sockets.push_back(socket);
            }
        }
        if (piece.boxes.empty()) {
            pool.warnings.push_back("piece '" + piece.name + "' has no boxes; skipped");
            continue;
        }
        pool.pieces.push_back(std::move(piece));
    }

    if (pool.pieces.empty() && pool.errors.empty()) {
        pool.errors.push_back("structure type '" + type + "' has no piece templates");
    }

    // conservative X/Z footprint half-extent (metres) of any assembled
    // structure relative to the site origin. Max over every piece of:
    //  - each box's max(|min|, |min + size|) on X and Z, and
    //  - each socket attach position on X and Z (sockets can translate jigsaw
    //    candidate pieces away from the origin during assembly).
    // long long + std::llabs keep the intermediates overflow-free; clamped to a
    // non-negative int. Used to pad the chunk AABB so boundary-straddling
    // structures are enumerated by every chunk they touch.
    {
        long long max_extent = 0;
        const auto consider = [&max_extent](long long value) {
            const long long a = std::llabs(value);
            if (a > max_extent) {
                max_extent = a;
            }
        };
        for (const StructurePiece& piece : pool.pieces) {
            for (const StructureBox& box : piece.boxes) {
                consider(static_cast<long long>(box.min.x));
                consider(static_cast<long long>(box.min.x) + box.size.x);
                consider(static_cast<long long>(box.min.z));
                consider(static_cast<long long>(box.min.z) + box.size.z);
            }
            for (const StructureSocket& socket : piece.sockets) {
                // A candidate piece's join socket coincides with a root socket
                // world position, so the candidate's far box corner can reach up
                // to (root socket reach) + (candidate piece extent). Bound it by
                // doubling the larger of the two contributing magnitudes; this is
                // a conservative superset (over-coverage only widens the disjoint
                // enumeration, never changes which voxels a chunk writes).
                consider(2ll * static_cast<long long>(socket.position.x));
                consider(2ll * static_cast<long long>(socket.position.z));
            }
        }
        if (max_extent > 1000000ll) {
            max_extent = 1000000ll; // sanity clamp; far beyond any real template
        }
        pool.footprint_radius = static_cast<int>(max_extent);
    }

    // Content hash over the canonicalized pool (placement params + pieces).
    u64 h = kFnvOffsetBasis;
    FnvMixValue(h, pool.spacing);
    FnvMixValue(h, pool.separation);
    FnvMixValue(h, pool.salt);
    FnvMixValue(h, pool.density);
    FnvMixValue(h, pool.footprint_radius);
    for (const StructurePiece& piece : pool.pieces) {
        FnvMix(h, piece.name.data(), piece.name.size());
        for (const StructureBox& box : piece.boxes) {
            FnvMixValue(h, box.min);
            FnvMixValue(h, box.size);
            FnvMixValue(h, box.material);
        }
        for (const StructureSocket& socket : piece.sockets) {
            FnvMix(h, socket.name.data(), socket.name.size());
            FnvMixValue(h, socket.position);
        }
    }
    pool.content_hash = h;
    return pool;
}

std::optional<StructureSite>
SiteInCell(const StructureTemplatePool& pool, int world_seed, int cell_x, int cell_z) {
    if (!pool.ok()) {
        return std::nullopt;
    }
    SplitMix64 rng(CellSeed(world_seed, pool.salt, cell_x, cell_z));
    // Density roll first so the rng stream advances identically whether or not
    // a site spawns (the jitter draws are still consumed below for determinism).
    const float roll = rng.next_unit();
    const int jitter_range = std::max(1, pool.spacing - 2 * pool.separation);
    const int jitter_x = pool.separation + rng.next_int(jitter_range);
    const int jitter_z = pool.separation + rng.next_int(jitter_range);
    if (roll >= pool.density) {
        return std::nullopt;
    }
    StructureSite site;
    site.type = pool.type;
    site.origin = IVec3(cell_x * pool.spacing + jitter_x, 0, cell_z * pool.spacing + jitter_z);
    // The assembly stream is seeded from the site origin so the same site always
    // assembles the same structure, independent of how it was queried.
    site.site_seed = CellSeed(world_seed, pool.salt ^ 0x5BD1E995u, site.origin.x, site.origin.z);
    return site;
}

std::optional<StructureSite> LocateNearestSite(const StructureTemplatePool& pool,
                                               int world_seed,
                                               int near_x,
                                               int near_z,
                                               int search_radius_cells) {
    if (!pool.ok()) {
        return std::nullopt;
    }
    const int cx = FloorDiv(near_x, pool.spacing);
    const int cz = FloorDiv(near_z, pool.spacing);
    std::optional<StructureSite> best;
    long long best_d2 = -1;
    for (int dz = -search_radius_cells; dz <= search_radius_cells; ++dz) {
        for (int dx = -search_radius_cells; dx <= search_radius_cells; ++dx) {
            auto site = SiteInCell(pool, world_seed, cx + dx, cz + dz);
            if (!site) {
                continue;
            }
            const long long ddx = static_cast<long long>(site->origin.x) - near_x;
            const long long ddz = static_cast<long long>(site->origin.z) - near_z;
            const long long d2 = ddx * ddx + ddz * ddz;
            if (best_d2 < 0 || d2 < best_d2) {
                best_d2 = d2;
                best = site;
            }
        }
    }
    return best;
}

std::vector<StructureSite> SitesInArea(
    const StructureTemplatePool& pool, int world_seed, int min_x, int min_z, int max_x, int max_z) {
    std::vector<StructureSite> sites;
    if (!pool.ok() || max_x <= min_x || max_z <= min_z) {
        return sites;
    }
    const int cx0 = FloorDiv(min_x, pool.spacing);
    const int cz0 = FloorDiv(min_z, pool.spacing);
    const int cx1 = FloorDiv(max_x - 1, pool.spacing);
    const int cz1 = FloorDiv(max_z - 1, pool.spacing);
    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            auto site = SiteInCell(pool, world_seed, cx, cz);
            if (site && site->origin.x >= min_x && site->origin.x < max_x &&
                site->origin.z >= min_z && site->origin.z < max_z) {
                sites.push_back(*site);
            }
        }
    }
    return sites;
}

namespace {

// Stamp a piece's boxes into the voxel accumulator at the given world offset.
void StampPiece(const StructurePiece& piece,
                const IVec3& world_offset,
                std::vector<StructureVoxel>& out) {
    for (const StructureBox& box : piece.boxes) {
        for (int dz = 0; dz < box.size.z; ++dz) {
            for (int dy = 0; dy < box.size.y; ++dy) {
                for (int dx = 0; dx < box.size.x; ++dx) {
                    StructureVoxel v;
                    v.position = IVec3(world_offset.x + box.min.x + dx,
                                       world_offset.y + box.min.y + dy,
                                       world_offset.z + box.min.z + dz);
                    v.material = box.material;
                    out.push_back(v);
                }
            }
        }
    }
}

} // namespace

std::vector<StructureVoxel> AssembleStructure(const StructureTemplatePool& pool,
                                              const StructureSite& site) {
    std::vector<StructureVoxel> voxels;
    if (!pool.ok()) {
        return voxels;
    }
    SplitMix64 rng(site.site_seed);

    // Deterministic root piece.
    const int root_index = rng.next_int(static_cast<int>(pool.pieces.size()));
    const StructurePiece& root = pool.pieces[root_index];
    StampPiece(root, site.origin, voxels);

    // Jigsaw: for each socket on the root, deterministically pick a candidate
    // piece and attach it so the candidate's FIRST socket coincides with the
    // root socket. The placement contract is translation-only, so piece
    // orientation remains exactly as authored.
    for (const StructureSocket& root_socket : root.sockets) {
        // Choose a piece that has at least one socket to join.
        const int candidate_index = rng.next_int(static_cast<int>(pool.pieces.size()));
        const StructurePiece& candidate = pool.pieces[candidate_index];
        if (candidate.sockets.empty()) {
            continue;
        }
        const StructureSocket& join = candidate.sockets.front();
        // World position of the root socket, minus the candidate's join offset,
        // gives the candidate's piece-origin in world space.
        const IVec3 root_socket_world = IVec3(site.origin.x + root_socket.position.x,
                                              site.origin.y + root_socket.position.y,
                                              site.origin.z + root_socket.position.z);
        const IVec3 candidate_offset = IVec3(root_socket_world.x - join.position.x,
                                             root_socket_world.y - join.position.y,
                                             root_socket_world.z - join.position.z);
        StampPiece(candidate, candidate_offset, voxels);
    }

    return voxels;
}

u64 ComputeAssembledVoxelHash(const std::vector<StructureVoxel>& voxels) {
    // Canonicalize: sort by (position, material) so the hash is independent of
    // emission order.
    std::vector<StructureVoxel> sorted = voxels;
    std::sort(sorted.begin(), sorted.end(), [](const StructureVoxel& a, const StructureVoxel& b) {
        if (a.position.x != b.position.x)
            return a.position.x < b.position.x;
        if (a.position.y != b.position.y)
            return a.position.y < b.position.y;
        if (a.position.z != b.position.z)
            return a.position.z < b.position.z;
        return a.material < b.material;
    });
    u64 h = kFnvOffsetBasis;
    FnvMixValue(h, static_cast<u64>(sorted.size()));
    for (const StructureVoxel& v : sorted) {
        FnvMixValue(h, v.position.x);
        FnvMixValue(h, v.position.y);
        FnvMixValue(h, v.position.z);
        FnvMixValue(h, v.material);
    }
    return h;
}

} // namespace Luminumbra::World
