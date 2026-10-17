#include "FarEditedCatalog.h"

#include "Chunk.h"
#include "FarAuthorityOverlay.h"
#include "FarEditedBake.h"
#include "FarTierTable.h"
#include "FarVolume.h"
#include "FarVolumeTile.h"
#include "luminumbra/core/Types.h"
#include "nlohmann/json.hpp"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Luminumbra::World {
namespace {
using Json = nlohmann::json;
using Error = FarCatalogError;
constexpr std::int64_t kCoordinateLimit = kFarVolumeExactCoordinateLimitMeters;

bool PositionEqual(const FarVolumePosition& a, const FarVolumePosition& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool ValidFootprint(const FarEditedFootprint& f) {
    return f.min.x >= -kCoordinateLimit && f.min.y >= -kCoordinateLimit &&
           f.min.z >= -kCoordinateLimit && f.max.x <= kCoordinateLimit &&
           f.max.y <= kCoordinateLimit && f.max.z <= kCoordinateLimit && f.min.x <= f.max.x &&
           f.min.y <= f.max.y && f.min.z <= f.max.z;
}
bool DependencyLess(const FarCatalogDependency& a, const FarCatalogDependency& b) {
    return std::tie(a.member_path, a.chunk_id) < std::tie(b.member_path, b.chunk_id);
}
bool ValidChunkIdentity(const FarCatalogDependency& d) {
    if (!ValidFootprint(d.footprint) || d.footprint.min.x % CHUNK_SIZE_X != 0 ||
        d.footprint.min.y % CHUNK_SIZE_Y != 0 || d.footprint.min.z % CHUNK_SIZE_Z != 0 ||
        d.footprint.max.x - d.footprint.min.x != CHUNK_SIZE_X ||
        d.footprint.max.y - d.footprint.min.y != CHUNK_SIZE_Y ||
        d.footprint.max.z - d.footprint.min.z != CHUNK_SIZE_Z)
        return false;
    const IVec3 coords{static_cast<int>(d.footprint.min.x / CHUNK_SIZE_X),
                       static_cast<int>(d.footprint.min.y / CHUNK_SIZE_Y),
                       static_cast<int>(d.footprint.min.z / CHUNK_SIZE_Z)};
    int rx = 0, rz = 0;
    Persistence::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
    return d.chunk_id == Chunk::calculate_id(coords) &&
           d.member_path ==
               Persistence::WorldSaveService::region_file_path({}, rx, rz).generic_string();
}
bool Keys(const Json& j, std::initializer_list<const char*> keys) {
    if (!j.is_object() || j.size() != keys.size())
        return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) { return j.contains(key); });
}
std::uint64_t Unsigned(const Json& j) {
    if (!j.is_number_unsigned() && (!j.is_number_integer() || j.get<std::int64_t>() < 0))
        throw std::invalid_argument("unsigned integer required");
    return j.get<std::uint64_t>();
}
std::int64_t Signed(const Json& j) {
    if (!j.is_number_integer() ||
        (j.is_number_unsigned() &&
         j.get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("signed integer required");
    return j.get<std::int64_t>();
}
FarVolumePosition Position(const Json& j) {
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("position required");
    return {Signed(j[0]), Signed(j[1]), Signed(j[2])};
}
Json PositionJson(const FarVolumePosition& p) {
    return Json::array({p.x, p.y, p.z});
}
Json DependencyJson(const FarCatalogDependency& d) {
    return {{"member", d.member_path},
            {"chunk", d.chunk_id},
            {"generation", d.member_generation},
            {"bytes", d.member_bytes},
            {"digest", d.member_digest},
            {"chunk_digest", d.chunk_digest},
            {"min", PositionJson(d.footprint.min)},
            {"max", PositionJson(d.footprint.max)}};
}
FarCatalogDependency Dependency(const Json& j) {
    if (!Keys(j,
              {"member", "chunk", "generation", "bytes", "digest", "chunk_digest", "min", "max"}))
        throw std::invalid_argument("dependency fields");
    FarCatalogDependency d;
    d.member_path = j.at("member").get<std::string>();
    d.chunk_id = Unsigned(j.at("chunk"));
    d.member_generation = Unsigned(j.at("generation"));
    d.member_bytes = Unsigned(j.at("bytes"));
    d.member_digest = Unsigned(j.at("digest"));
    d.chunk_digest = Unsigned(j.at("chunk_digest"));
    d.footprint = {Position(j.at("min")), Position(j.at("max"))};
    return d;
}
Json ReceiptJson(const FarCatalogReceipt& r) {
    Json deps = Json::array();
    for (const auto& d : r.dependencies)
        deps.push_back(DependencyJson(d));
    return {{"tier", r.key.tier},
            {"tx", r.key.tx},
            {"tz", r.key.tz},
            {"path", r.path},
            {"source_generation", r.source_generation},
            {"overlay_bytes", r.overlay_bytes},
            {"overlay_fingerprint", r.overlay_fingerprint},
            {"terrain_identity", r.terrain_identity},
            {"content_identity", r.content_identity},
            {"numeric", static_cast<unsigned>(r.numeric)},
            {"caves", static_cast<unsigned>(r.caves)},
            {"span", Json::array({r.edited_span.min_y, r.edited_span.max_y})},
            {"dependencies", std::move(deps)}};
}
FarCatalogReceipt Receipt(const Json& j, const FarCatalogLimits& limits, std::size_t& count) {
    if (!Keys(j,
              {"tier",
               "tx",
               "tz",
               "path",
               "source_generation",
               "overlay_bytes",
               "overlay_fingerprint",
               "terrain_identity",
               "content_identity",
               "numeric",
               "caves",
               "span",
               "dependencies"}))
        throw std::invalid_argument("receipt fields");
    const auto tier = Unsigned(j.at("tier"));
    const auto numeric = Unsigned(j.at("numeric"));
    const auto caves = Unsigned(j.at("caves"));
    if (tier < 1 || tier > kFarTierCount ||
        numeric != static_cast<unsigned>(FarVolumeNumericProfile::LegacySigned16) ||
        caves > static_cast<unsigned>(FarCaveMode::BoxFiltered))
        throw std::invalid_argument("policy or tier");
    FarCatalogReceipt r;
    r.key = {static_cast<std::uint8_t>(tier), Signed(j.at("tx")), Signed(j.at("tz"))};
    r.path = j.at("path").get<std::string>();
    r.source_generation = Unsigned(j.at("source_generation"));
    r.overlay_bytes = Unsigned(j.at("overlay_bytes"));
    r.overlay_fingerprint = Unsigned(j.at("overlay_fingerprint"));
    r.terrain_identity = Unsigned(j.at("terrain_identity"));
    r.content_identity = Unsigned(j.at("content_identity"));
    r.numeric = static_cast<FarVolumeNumericProfile>(numeric);
    r.caves = static_cast<FarCaveMode>(caves);
    const auto& span = j.at("span");
    if (!span.is_array() || span.size() != 2)
        throw std::invalid_argument("span");
    r.edited_span = {Signed(span[0]), Signed(span[1])};
    const auto& deps = j.at("dependencies");
    if (!deps.is_array() || deps.size() > limits.max_dependencies - count)
        throw std::length_error("dependencies");
    count += deps.size();
    r.dependencies.reserve(deps.size());
    for (const auto& d : deps)
        r.dependencies.push_back(Dependency(d));
    return r;
}
Error ValidateReceipt(const FarCatalogReceipt& r,
                      const FarCatalogLimits& limits,
                      std::size_t& count) {
    const auto tier = FarTierAt(r.key.tier);
    if (!tier.has_value() || r.key.tx < -kFsv1MaxAbsTileIndex || r.key.tx > kFsv1MaxAbsTileIndex ||
        r.key.tz < -kFsv1MaxAbsTileIndex || r.key.tz > kFsv1MaxAbsTileIndex)
        return Error::Malformed;
    if (r.path != FarOverlayPath({}, r.key).generic_string())
        return Error::PathMismatch;
    if (r.overlay_bytes < kFsv1HeaderBytes + kFsv1TrailerBytes ||
        r.overlay_bytes > kFsv1MaxFileBytes ||
        r.numeric != FarVolumeNumericProfile::LegacySigned16 ||
        (r.caves != FarCaveMode::BandLimited && r.caves != FarCaveMode::BoxFiltered) ||
        r.edited_span.min_y >= r.edited_span.max_y || r.edited_span.min_y < -kCoordinateLimit ||
        r.edited_span.max_y > kCoordinateLimit ||
        r.edited_span.min_y % tier->brick_edge_meters != 0 ||
        r.edited_span.max_y % tier->brick_edge_meters != 0)
        return Error::Malformed;
    if (r.dependencies.size() > limits.max_dependencies - count)
        return Error::LimitExceeded;
    count += r.dependencies.size();
    for (std::size_t i = 0; i < r.dependencies.size(); ++i) {
        const auto& d = r.dependencies[i];
        const auto cls = Persistence::ClassifySnapshotMember(d.member_path);
        if (!cls.has_value() || *cls != Persistence::MemberClass::Authoritative ||
            !d.member_path.starts_with("chunks/region/r.") || d.member_path.size() > 128 ||
            d.member_generation > r.source_generation || d.member_bytes == 0 ||
            !ValidChunkIdentity(d))
            return Error::Malformed;
        if (i != 0 && !DependencyLess(r.dependencies[i - 1], d))
            return Error::Ordering;
    }
    return Error::None;
}
} // namespace

bool SameFarCatalogDependency(const FarCatalogDependency& a, const FarCatalogDependency& b) {
    return a.member_path == b.member_path && a.chunk_id == b.chunk_id &&
           a.member_generation == b.member_generation && a.member_bytes == b.member_bytes &&
           a.member_digest == b.member_digest && a.chunk_digest == b.chunk_digest &&
           PositionEqual(a.footprint.min, b.footprint.min) &&
           PositionEqual(a.footprint.max, b.footprint.max);
}
FarCatalogError ValidateFarEditedCatalog(const FarEditedCatalog& catalog,
                                         const FarCatalogLimits& limits) {
    if (limits.max_bytes > Persistence::kMaxSnapshotIndexBytes ||
        limits.max_tiles > Persistence::kMaxSnapshotMembers ||
        limits.max_dependencies > Persistence::kMaxSnapshotMembers ||
        catalog.tiles.size() > limits.max_tiles || (!catalog.present && !catalog.tiles.empty()))
        return Error::LimitExceeded;
    std::size_t count = 0;
    for (std::size_t i = 0; i < catalog.tiles.size(); ++i) {
        if (i != 0 && !(catalog.tiles[i - 1].key < catalog.tiles[i].key))
            return Error::Ordering;
        const auto error = ValidateReceipt(catalog.tiles[i], limits, count);
        if (error != Error::None)
            return error;
    }
    return Error::None;
}
FarCatalogError EncodeFarEditedCatalog(const FarEditedCatalog& catalog,
                                       const FarCatalogLimits& limits,
                                       std::string& out) {
    const auto error = ValidateFarEditedCatalog(catalog, limits);
    if (error != Error::None)
        return error;
    try {
        Json tiles = Json::array();
        for (const auto& r : catalog.tiles)
            tiles.push_back(ReceiptJson(r));
        auto bytes = Json{{"schema", kFarCatalogSchema}, {"tiles", std::move(tiles)}}.dump() + "\n";
        if (bytes.size() > limits.max_bytes)
            return Error::LimitExceeded;
        out = std::move(bytes);
        return Error::None;
    } catch (const std::exception&) {
        return Error::LimitExceeded;
    }
}
FarCatalogError DecodeFarEditedCatalog(std::string_view bytes,
                                       const FarCatalogLimits& limits,
                                       FarEditedCatalog& out) {
    if (bytes.size() > std::min(limits.max_bytes, Persistence::kMaxSnapshotIndexBytes))
        return Error::LimitExceeded;
    try {
        // Reject duplicate object keys and excessive nesting before constructing a DOM.
        std::vector<std::set<std::string>> keys;
        bool valid = true;
        const auto parsed =
            Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
                if (depth > 8)
                    valid = false;
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                if (event == Json::parse_event_t::key && !keys.empty() &&
                    !keys.back().insert(value.get<std::string>()).second)
                    valid = false;
                if (event == Json::parse_event_t::object_end && !keys.empty())
                    keys.pop_back();
                return valid;
            });
        if (!valid || !Keys(parsed, {"schema", "tiles"}))
            return Error::Malformed;
        if (parsed.at("schema") != kFarCatalogSchema)
            return Error::FutureVersion;
        const auto& tiles = parsed.at("tiles");
        if (!tiles.is_array())
            return Error::Malformed;
        if (tiles.size() > limits.max_tiles)
            return Error::LimitExceeded;
        FarEditedCatalog result;
        result.present = true;
        result.tiles.reserve(tiles.size());
        std::size_t count = 0;
        for (const auto& r : tiles)
            result.tiles.push_back(Receipt(r, limits, count));
        const auto error = ValidateFarEditedCatalog(result, limits);
        if (error == Error::None)
            out = std::move(result);
        return error;
    } catch (const std::length_error&) {
        return Error::LimitExceeded;
    } catch (const std::exception&) {
        return Error::Malformed;
    }
}
namespace {
FarCatalogError ReadCatalog(const std::filesystem::path& root,
                            const FarCatalogLimits& limits,
                            FarEditedCatalog& out,
                            Persistence::MemberDigest* digest = nullptr) {
    if (digest)
        *digest = {};
    try {
        const auto path = root / "far/catalog.json";
        if (!std::filesystem::exists(path)) {
            out = {};
            return Error::None;
        }
        if (!std::filesystem::is_regular_file(path))
            return Error::IoError;
        const auto size = std::filesystem::file_size(path);
        if (size > std::min(limits.max_bytes, Persistence::kMaxSnapshotIndexBytes))
            return Error::LimitExceeded;
        // At most 16 MiB; validate and fingerprint the exact bytes from one regular handle.
        std::string bytes;
        if (!Persistence::read_bounded_save_file(
                path,
                bytes,
                std::min(limits.max_bytes, Persistence::kMaxSnapshotIndexBytes),
                nullptr))
            return Error::IoError;
        const auto result = DecodeFarEditedCatalog(bytes, limits, out);
        if (result == Error::None && digest) {
            digest->bytes = bytes.size();
            digest->fnv64 = Persistence::fnv1a64_bytes(bytes.data(), bytes.size());
            digest->present = true;
        }
        return result;
    } catch (const std::exception&) {
        return Error::IoError;
    }
}
bool CatalogDigest(const std::filesystem::path& root,
                   std::string_view path,
                   Persistence::MemberDigest& out,
                   std::string& error,
                   bool prepared) {
    if (path != "far/catalog.json") {
        error = "catalog path mismatch";
        return false;
    }
    FarEditedCatalog catalog;
    Persistence::MemberDigest digest;
    if (ReadCatalog(root, {}, catalog, &digest) != Error::None) {
        error = "invalid far catalog";
        return false;
    }
    Persistence::SnapshotIndex index;
    if (!Persistence::read_snapshot_index(root, index, nullptr)) {
        error = "invalid snapshot index";
        return false;
    }
    const auto* transaction = Persistence::SaveTransaction::active_for(root);
    const auto generation =
        prepared && transaction ? transaction->new_generation() : index.generation;
    for (const auto& r : catalog.tiles) {
        if (generation != 0 && r.source_generation > generation) {
            error = "future far catalog source generation";
            return false;
        }
    }
    out = digest;
    return true;
}
} // namespace
FarCatalogError ReadFarEditedCatalog(const std::filesystem::path& root,
                                     const FarCatalogLimits& limits,
                                     FarEditedCatalog& out) {
    return ReadCatalog(root, limits, out);
}
bool FarEditedCatalogDigest(const std::filesystem::path& root,
                            std::string_view path,
                            Persistence::MemberDigest& out,
                            std::string& error) {
    return CatalogDigest(root, path, out, error, true);
}
bool InspectFarEditedCatalogDigest(const std::filesystem::path& root,
                                   std::string_view path,
                                   Persistence::MemberDigest& out,
                                   std::string& error) {
    return CatalogDigest(root, path, out, error, false);
}
} // namespace Luminumbra::World
