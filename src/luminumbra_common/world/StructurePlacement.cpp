#include "StructurePlacement.h"

#include "../persistence/SavedInputFile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "nlohmann/json.hpp"

#include "../core/FilesystemPath.h"
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
            if (warnings.size() >= 128)
                throw std::runtime_error("structure warning limit");
            warnings.push_back("unknown key '" + it.key() + "' in " + context);
        }
    }
}

int ParseCoordinate(const nlohmann::json& value) {
    if (!value.is_number_integer() || value < -kMaxStructureCoordinate ||
        value > kMaxStructureCoordinate)
        throw std::runtime_error("structure coordinate out of range");
    return value.get<int>();
}

IVec3 ParseIVec3(const nlohmann::json& arr, const IVec3& fallback) {
    if (arr.is_null() || (arr.is_array() && arr.empty()))
        return fallback;
    if (!arr.is_array() || arr.size() != 3)
        throw std::runtime_error("invalid structure coordinate");
    return IVec3(ParseCoordinate(arr[0]), ParseCoordinate(arr[1]), ParseCoordinate(arr[2]));
}

bool CountPiece(const StructurePiece& piece, const IVec3& offset, std::size_t& count);

nlohmann::json ParseTemplate(const std::string& bytes) {
    std::size_t nodes = 0;
    return nlohmann::json::parse(
        bytes, [&nodes](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 32 || ++nodes > 65536)
                throw std::runtime_error("structure parser work limit");
            return true;
        });
}

bool ValidPool(const StructureTemplatePool& pool) {
    return pool.ok() && pool.type.size() <= 1024 && pool.pieces.size() <= kMaxStructurePieces &&
           pool.spacing > 0 && pool.spacing <= kMaxStructureCoordinate && pool.separation >= 0 &&
           pool.separation < pool.spacing && std::isfinite(pool.density) && pool.density >= 0.0f &&
           pool.density <= 1.0f;
}

bool Coordinate(std::int64_t value) {
    return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}

} // namespace

bool ReadStructureTemplateFile(const std::filesystem::path& path, std::string& bytes) {
    constexpr std::size_t maximum = 1024u * 1024u;
    if (Filesystem::capture_content_paths().active())
        return Filesystem::capture_content_paths().read_file(
            path, bytes, maximum, Filesystem::MissingContent::Optional);
    return Persistence::SavedInputFile(path, Persistence::SavedInputFile::LeafSymlinks::Follow)
        .read(bytes, maximum);
}

StructureTemplatePool LoadStructureTemplatePool(const std::filesystem::path& type_dir,
                                                const std::string& type,
                                                const WorldgenFileReader& read_file) {
    StructureTemplatePool pool;
    if (type.size() > 1024 || type_dir.native().size() > 32768) {
        pool.errors.push_back("structure path or type limit");
        return pool;
    }
    pool.type = type;
    std::size_t pool_bytes = 0;
    const auto bounded_read = [&read_file, &pool_bytes](const auto& path, std::string& bytes) {
        if (!read_file(path, bytes) || bytes.size() > 1024u * 1024u ||
            bytes.size() > 4u * 1024u * 1024u - pool_bytes)
            return false;
        pool_bytes += bytes.size();
        return true;
    };
    try {
        std::error_code ec;
        if (!std::filesystem::is_directory(type_dir, ec)) {
            pool.io_failed = bool(ec);
            pool.errors.push_back("structure type dir not found: " +
                                  Filesystem::GenericUtf8PathString(type_dir));
            return pool;
        }

        // placement.json (optional; defaults documented in the header).
        const std::filesystem::path placement_path = type_dir / "placement.json";
        // Match the preset manifest and historical direct loader: dangling optional
        // links use defaults; present links are verified and read through one handle.
        const bool placement_present = std::filesystem::exists(placement_path, ec);
        if (ec) {
            pool.io_failed = true;
            throw std::runtime_error("unreadable placement.json");
        }
        if (placement_present) {
            std::string bytes;
            if (!bounded_read(placement_path, bytes)) {
                pool.io_failed = true;
                pool.errors.push_back("failed to read placement.json");
                return pool;
            }
            nlohmann::json data;
            try {
                data = ParseTemplate(bytes);
            } catch (const nlohmann::json::parse_error& e) {
                pool.errors.push_back(std::string("placement.json parse error: ") + e.what());
                return pool;
            }
            WarnUnknownKeys(data,
                            {"spacing", "separation", "salt", "density", "_comment"},
                            "placement.json",
                            pool.warnings);
            pool.spacing =
                data.contains("spacing") ? ParseCoordinate(data["spacing"]) : pool.spacing;
            pool.separation =
                data.contains("separation") ? ParseCoordinate(data["separation"]) : pool.separation;
            if (data.contains("salt") && (!data["salt"].is_number_integer() || data["salt"] < 0 ||
                                          data["salt"] > std::numeric_limits<u32>::max()))
                throw std::runtime_error("invalid structure salt");
            pool.salt = data.value("salt", pool.salt);
            pool.density = data.value("density", pool.density);
            if (pool.spacing <= 0 || pool.separation < 0 || pool.separation >= pool.spacing ||
                !std::isfinite(pool.density) || pool.density < 0.0f || pool.density > 1.0f)
                throw std::runtime_error("invalid structure placement");
        }

        ec.clear();

        // Every *.json under the type dir EXCEPT placement.json is a piece. Sort by
        // UTF-8 logical filename, matching preset manifest ordering on every platform.
        std::vector<std::filesystem::path> piece_files;
        std::filesystem::directory_iterator entry_it(type_dir, ec), end;
        std::size_t probes = 0;
        for (; !ec && entry_it != end; entry_it.increment(ec)) {
            if (++probes > 256)
                throw std::runtime_error("structure directory work limit");
            const auto& entry = *entry_it;
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto& p = entry.path();
            if (p.extension() != ".json" || p.filename() == "placement.json") {
                continue;
            }
            if (piece_files.size() >= kMaxStructurePieces)
                throw std::runtime_error("structure piece limit");
            piece_files.push_back(p);
        }
        if (ec) {
            pool.io_failed = true;
            pool.errors.push_back("failed to enumerate structure pieces: " + ec.message());
            return pool;
        }
        std::sort(piece_files.begin(), piece_files.end(), [](const auto& a, const auto& b) {
            return Filesystem::GenericUtf8PathString(a.filename()) <
                   Filesystem::GenericUtf8PathString(b.filename());
        });

        for (const auto& piece_path : piece_files) {
            std::string bytes;
            if (!bounded_read(piece_path, bytes)) {
                pool.io_failed = true;
                pool.errors.push_back("failed to read structure piece: " +
                                      Filesystem::GenericUtf8PathString(piece_path.filename()));
                continue;
            }
            nlohmann::json data;
            try {
                data = ParseTemplate(bytes);
            } catch (const nlohmann::json::parse_error& e) {
                pool.errors.push_back(Filesystem::GenericUtf8PathString(piece_path.filename()) +
                                      std::string(" parse error: ") + e.what());
                continue;
            }
            WarnUnknownKeys(data,
                            {"name", "boxes", "sockets", "_comment"},
                            Filesystem::GenericUtf8PathString(piece_path.filename()),
                            pool.warnings);

            StructurePiece piece;
            piece.name = data.value("name", Filesystem::GenericUtf8PathString(piece_path.stem()));
            if (data.contains("boxes") && data["boxes"].is_array()) {
                if (data["boxes"].size() > kMaxStructureBoxes)
                    throw std::runtime_error("structure box limit");
                for (const auto& box_json : data["boxes"]) {
                    StructureBox box;
                    box.min =
                        ParseIVec3(box_json.value("min", nlohmann::json::array()), IVec3(0, 0, 0));
                    box.size =
                        ParseIVec3(box_json.value("size", nlohmann::json::array()), IVec3(1, 1, 1));
                    if (box.size.x <= 0 || box.size.y <= 0 || box.size.z <= 0)
                        throw std::runtime_error("invalid structure box size");
                    const auto material = box_json.value("material", nlohmann::json(0));
                    if (!material.is_number_integer() || material < 0 || material > 255)
                        throw std::runtime_error("invalid structure material");
                    box.material = material.get<u8>();
                    piece.boxes.push_back(box);
                }
            }
            if (data.contains("sockets") && data["sockets"].is_array()) {
                if (data["sockets"].size() > kMaxStructureSockets)
                    throw std::runtime_error("structure socket limit");
                for (const auto& socket_json : data["sockets"]) {
                    StructureSocket socket;
                    socket.name = socket_json.value("name", std::string());
                    socket.position = ParseIVec3(
                        socket_json.value("position", nlohmann::json::array()), IVec3(0, 0, 0));
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

        std::size_t expanded = 0;
        for (const auto& piece : pool.pieces)
            if (!CountPiece(piece, IVec3(0), expanded))
                throw std::runtime_error("structure expansion limit");

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
    } catch (const std::exception& error) {
        pool.errors.push_back(error.what());
        pool.pieces.clear();
        pool.content_hash = 0;
    }
    return pool;
}

std::optional<StructureSite>
SiteInCell(const StructureTemplatePool& pool, int world_seed, int cell_x, int cell_z) {
    if (!ValidPool(pool)) {
        return std::nullopt;
    }
    SplitMix64 rng(CellSeed(world_seed, pool.salt, cell_x, cell_z));
    // Density roll first so the rng stream advances identically whether or not
    // a site spawns (the jitter draws are still consumed below for determinism).
    const float roll = rng.next_unit();
    const int jitter_range =
        std::max(1,
                 static_cast<int>(std::max<std::int64_t>(
                     1, static_cast<std::int64_t>(pool.spacing) - 2ll * pool.separation)));
    const int jitter_x = pool.separation + rng.next_int(jitter_range);
    const int jitter_z = pool.separation + rng.next_int(jitter_range);
    if (roll >= pool.density) {
        return std::nullopt;
    }
    StructureSite site;
    site.type = pool.type;
    const auto x = static_cast<std::int64_t>(cell_x) * pool.spacing + jitter_x;
    const auto z = static_cast<std::int64_t>(cell_z) * pool.spacing + jitter_z;
    if (!Coordinate(x) || !Coordinate(z))
        return std::nullopt;
    site.origin = IVec3(static_cast<int>(x), 0, static_cast<int>(z));
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
    if (!ValidPool(pool) || search_radius_cells < 0 || search_radius_cells > 31) {
        return std::nullopt;
    }
    const int cx = FloorDiv(near_x, pool.spacing);
    const int cz = FloorDiv(near_z, pool.spacing);
    std::optional<StructureSite> best;
    std::optional<std::pair<bool, std::uint64_t>> best_d2;
    for (int dz = -search_radius_cells; dz <= search_radius_cells; ++dz) {
        for (int dx = -search_radius_cells; dx <= search_radius_cells; ++dx) {
            const auto cell_x = static_cast<std::int64_t>(cx) + dx;
            const auto cell_z = static_cast<std::int64_t>(cz) + dz;
            if (!Coordinate(cell_x) || !Coordinate(cell_z))
                continue;
            auto site =
                SiteInCell(pool, world_seed, static_cast<int>(cell_x), static_cast<int>(cell_z));
            if (!site) {
                continue;
            }
            const long long ddx = static_cast<long long>(site->origin.x) - near_x;
            const long long ddz = static_cast<long long>(site->origin.z) - near_z;
            const auto x_distance = static_cast<std::uint64_t>(std::llabs(ddx));
            const auto z_distance = static_cast<std::uint64_t>(std::llabs(ddz));
            const auto x2 = x_distance * x_distance;
            const auto z2 = z_distance * z_distance;
            const std::pair d2{x2 > std::numeric_limits<std::uint64_t>::max() - z2, x2 + z2};
            if (!best_d2 || d2 < *best_d2) {
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
    if (!ValidPool(pool) || max_x <= min_x || max_z <= min_z) {
        return sites;
    }
    const int cx0 = FloorDiv(min_x, pool.spacing);
    const int cz0 = FloorDiv(min_z, pool.spacing);
    const int cx1 = FloorDiv(max_x - 1, pool.spacing);
    const int cz1 = FloorDiv(max_z - 1, pool.spacing);
    const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(cx1) - cx0 + 1);
    const auto depth = static_cast<std::uint64_t>(static_cast<std::int64_t>(cz1) - cz0 + 1);
    if (width > kMaxStructureSites / depth)
        return sites;
    for (auto cz = static_cast<std::int64_t>(cz0); cz <= cz1; ++cz) {
        for (auto cx = static_cast<std::int64_t>(cx0); cx <= cx1; ++cx) {
            auto site = SiteInCell(pool, world_seed, static_cast<int>(cx), static_cast<int>(cz));
            if (site && site->origin.x >= min_x && site->origin.x < max_x &&
                site->origin.z >= min_z && site->origin.z < max_z) {
                sites.push_back(*site);
            }
        }
    }
    return sites;
}

namespace {

// Validate all arithmetic and the exact expanded count before any voxel allocation.
bool CountPiece(const StructurePiece& piece, const IVec3& offset, std::size_t& count) {
    if (piece.boxes.empty() || piece.boxes.size() > kMaxStructureBoxes ||
        piece.sockets.size() > kMaxStructureSockets || piece.name.size() > 1024)
        return false;
    for (const auto& socket : piece.sockets) {
        if (socket.name.size() > 1024)
            return false;
        for (int axis = 0; axis < 3; ++axis)
            if (socket.position[axis] < -kMaxStructureCoordinate ||
                socket.position[axis] > kMaxStructureCoordinate)
                return false;
    }
    for (const auto& box : piece.boxes) {
        std::size_t volume = 1;
        for (int axis = 0; axis < 3; ++axis) {
            const int size = box.size[axis];
            if (size <= 0 || static_cast<std::size_t>(size) > kMaxStructureVoxels / volume ||
                box.min[axis] < -kMaxStructureCoordinate || box.min[axis] > kMaxStructureCoordinate)
                return false;
            volume *= static_cast<std::size_t>(size);
            const auto first = static_cast<std::int64_t>(offset[axis]) + box.min[axis];
            if (!Coordinate(first) || !Coordinate(first + size - 1))
                return false;
        }
        if (volume > kMaxStructureVoxels - count)
            return false;
        count += volume;
    }
    return true;
}

void StampPiece(const StructurePiece& piece,
                const IVec3& world_offset,
                std::vector<StructureVoxel>& out) {
    for (const auto& box : piece.boxes)
        for (int dz = 0; dz < box.size.z; ++dz)
            for (int dy = 0; dy < box.size.y; ++dy)
                for (int dx = 0; dx < box.size.x; ++dx)
                    out.push_back(
                        {IVec3(static_cast<int>(static_cast<std::int64_t>(world_offset.x) +
                                                box.min.x + dx),
                               static_cast<int>(static_cast<std::int64_t>(world_offset.y) +
                                                box.min.y + dy),
                               static_cast<int>(static_cast<std::int64_t>(world_offset.z) +
                                                box.min.z + dz)),
                         box.material});
}

} // namespace

std::vector<StructureVoxel> AssembleStructure(const StructureTemplatePool& pool,
                                              const StructureSite& site) {
    std::vector<StructureVoxel> voxels;
    if (!ValidPool(pool))
        return voxels;
    std::size_t template_voxels = 0;
    for (const auto& piece : pool.pieces)
        if (!CountPiece(piece, IVec3(0), template_voxels))
            return voxels;
    SplitMix64 rng(site.site_seed);
    const StructurePiece& root =
        pool.pieces[static_cast<std::size_t>(rng.next_int(static_cast<int>(pool.pieces.size())))];
    struct Placement {
        const StructurePiece* piece = nullptr;
        IVec3 offset{0};
    };
    // One root plus at most 32 socket attachments. No recursive expansion.
    std::array<Placement, kMaxStructureSockets + 1> placements{};
    placements[0] = {&root, site.origin};
    std::size_t count = 1;
    for (const auto& socket : root.sockets) {
        const auto& candidate = pool.pieces[static_cast<std::size_t>(
            rng.next_int(static_cast<int>(pool.pieces.size())))];
        if (candidate.sockets.empty())
            continue;
        IVec3 offset(0);
        for (int axis = 0; axis < 3; ++axis) {
            const auto value = static_cast<std::int64_t>(site.origin[axis]) +
                               socket.position[axis] - candidate.sockets.front().position[axis];
            if (!Coordinate(value))
                return voxels;
            offset[axis] = static_cast<int>(value);
        }
        placements[count++] = {&candidate, offset};
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (!CountPiece(*placements[i].piece, placements[i].offset, total))
            return voxels;
    // Exactly the checked output size; no vector growth beyond the 262144-voxel bound.
    voxels.reserve(total);
    for (std::size_t i = 0; i < count; ++i)
        StampPiece(*placements[i].piece, placements[i].offset, voxels);
    return voxels;
}

u64 ComputeAssembledVoxelHash(const std::vector<StructureVoxel>& voxels) {
    // Canonicalize: sort by (position, material) so the hash is independent of
    // emission order.
    if (voxels.size() > kMaxStructureVoxels)
        return 0;
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
