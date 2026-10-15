#include "EditedSourceImage.h"

#include "WorldSaveService.h"
#include "nlohmann/json.hpp"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/FarVolume.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <new>
#include <utility>

namespace Luminumbra::Persistence {
namespace {
using Error = EditedSourceError;
using Position = World::FarVolumePosition;
using Footprint = World::FarEditedFootprint;
constexpr std::size_t kSide = CHUNK_SIZE_X + 1;
constexpr std::size_t kSamples = kSide * kSide * kSide;
constexpr std::int64_t kCoordinateLimit = World::kFarVolumeExactCoordinateLimitMeters;
constexpr std::uint64_t kPathBytes = 128;

struct Reservation {
    explicit Reservation(std::shared_ptr<World::FarVolumeMemoryPool> pool)
        : memory(std::move(pool))
        , lease(*memory) {}
    std::shared_ptr<World::FarVolumeMemoryPool> memory;
    World::FarVolumeMemoryLease lease;
};
struct Payload {
    std::vector<float> density;
    std::vector<std::uint8_t> material;
};

std::array<std::int64_t, 3> Axes(const Position& position) {
    return {position.x, position.y, position.z};
}
Footprint ChunkFootprint(const Chunk& chunk) {
    const auto& c = chunk.get_coords();
    const Position origin{static_cast<std::int64_t>(c.x) * CHUNK_SIZE_X,
                          static_cast<std::int64_t>(c.y) * CHUNK_SIZE_Y,
                          static_cast<std::int64_t>(c.z) * CHUNK_SIZE_Z};
    return {origin, {origin.x + CHUNK_SIZE_X, origin.y + CHUNK_SIZE_Y, origin.z + CHUNK_SIZE_Z}};
}
bool Intersects(const Footprint& a, const Footprint& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
bool Selected(const Footprint& footprint, const EditedSourceSelection& selection) {
    for (const auto& tile : selection.tiles) {
        if (Intersects(footprint, tile.support))
            return true;
        const auto spacing = World::kFarTierTable[tile.key.tier - 1].sample_spacing_meters;
        for (int axis = 0; axis < 3; ++axis) {
            for (const auto direction : {-1, 1}) {
                auto shifted = tile.support;
                const auto displacement = static_cast<std::int64_t>(spacing) * direction;
                if (axis == 0) {
                    shifted.min.x += displacement;
                    shifted.max.x += displacement;
                } else if (axis == 1) {
                    shifted.min.y += displacement;
                    shifted.max.y += displacement;
                } else {
                    shifted.min.z += displacement;
                    shifted.max.z += displacement;
                }
                if (Intersects(footprint, shifted))
                    return true;
            }
        }
    }
    return false;
}
Error ValidateSelection(const EditedSourceSelection& selection, const EditedSourceLimits& limits) {
    if (selection.tiles.size() > limits.max_tiles)
        return Error::WorkLimit;
    for (std::size_t i = 0; i < selection.tiles.size(); ++i) {
        const auto& tile = selection.tiles[i];
        const auto tier = World::FarTierAt(tile.key.tier);
        if (!tier)
            return Error::InvalidSelection;
        const auto lo = Axes(tile.support.min);
        const auto hi = Axes(tile.support.max);
        for (std::size_t axis = 0; axis < lo.size(); ++axis) {
            if (lo[axis] <= -static_cast<std::int64_t>(kCoordinateLimit) ||
                hi[axis] >= static_cast<std::int64_t>(kCoordinateLimit) || lo[axis] > hi[axis] ||
                lo[axis] % tier->sample_spacing_meters != 0 ||
                hi[axis] % tier->sample_spacing_meters != 0)
                return Error::InvalidSelection;
        }
        const auto edge = static_cast<std::int64_t>(tier->tile_edge_meters);
        if (tile.key.tx <= -static_cast<std::int64_t>(kCoordinateLimit) / edge ||
            tile.key.tx >= static_cast<std::int64_t>(kCoordinateLimit) / edge ||
            tile.key.tz <= -static_cast<std::int64_t>(kCoordinateLimit) / edge ||
            tile.key.tz >= static_cast<std::int64_t>(kCoordinateLimit) / edge ||
            tile.support.min.x != tile.key.tx * edge ||
            tile.support.max.x != (tile.key.tx + 1) * edge ||
            tile.support.min.z != tile.key.tz * edge ||
            tile.support.max.z != (tile.key.tz + 1) * edge)
            return Error::InvalidSelection;
        if (i != 0 && !(selection.tiles[i - 1].key < tile.key))
            return Error::InvalidSelection;
    }
    return Error::None;
}

bool Finite(const std::vector<float>& values) {
    return std::all_of(
        values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}
Error ValidateChunk(const Chunk& chunk) {
    if (chunk.sdf_data.size() != kSamples ||
        (!chunk.material_data.empty() && chunk.material_data.size() != kSamples) ||
        !Finite(chunk.sdf_data) || !Finite(chunk.water_level_data) ||
        !Finite(chunk.water_sim_terrain_height) || !Finite(chunk.water_rest_level) ||
        !std::isfinite(chunk.max_water_delta_last_tick))
        return Error::InvalidRecord;
    for (const auto& flow : chunk.water_flow_data) {
        if (!std::isfinite(flow.x) || !std::isfinite(flow.y))
            return Error::InvalidRecord;
    }
    Systems::WaterSystem::ValidateRegionalGrid(chunk, true);
    return Error::None;
}
bool PayloadBytes(const Chunk& chunk, std::uint64_t limit, std::uint64_t& out) {
    const std::array<std::pair<std::size_t, std::size_t>, 10> arrays{{
        {chunk.sdf_data.size(), sizeof(float)},
        {chunk.material_data.size(), sizeof(std::uint8_t)},
        {chunk.water_level_data.size(), sizeof(float)},
        {chunk.water_sim_terrain_height.size(), sizeof(float)},
        {chunk.water_rest_level.size(), sizeof(float)},
        {chunk.water_flow_data.size(), sizeof(Vec2)},
        {chunk.water_depth_mm.size(), sizeof(std::int32_t)},
        {chunk.water_bed_mm.size(), sizeof(std::int32_t)},
        {chunk.water_edge_flux.size(), sizeof(std::int32_t)},
        {chunk.water_src_mm.size(), sizeof(std::int32_t)},
    }};
    std::uint64_t bytes = 0;
    for (const auto& [count, width] : arrays) {
        if (count > (limit - bytes) / width)
            return false;
        bytes += static_cast<std::uint64_t>(count) * width;
    }
    out = bytes;
    return true;
}
EditedSourceWater CopyWater(const Chunk& chunk) {
    EditedSourceWater water;
    water.chunk_id = chunk.get_id();
    water.has_sim = chunk.has_water_sim.load(std::memory_order_acquire);
    water.sleeping = chunk.is_water_sleeping.load(std::memory_order_acquire);
    water.resolution = chunk.current_water_resolution.load(std::memory_order_acquire);
    water.ticks_below_threshold = chunk.ticks_below_threshold;
    water.max_delta = chunk.max_water_delta_last_tick;
    water.level = chunk.water_level_data;
    water.terrain_height = chunk.water_sim_terrain_height;
    water.rest_level = chunk.water_rest_level;
    water.flow = chunk.water_flow_data;
    water.depth_mm = chunk.water_depth_mm;
    water.bed_mm = chunk.water_bed_mm;
    water.edge_flux = chunk.water_edge_flux;
    water.source_mm = chunk.water_src_mm;
    return water;
}

bool SameDigest(const SnapshotMember& member, const MemberDigest& actual) {
    return member.present == actual.present &&
           member.bytes == (actual.present ? actual.bytes : 0) &&
           member.fnv64 == (actual.present ? actual.fnv64 : 0);
}
bool RegionPath(std::string_view path) {
    return path.rfind("chunks/region/r.", 0) == 0 && member_path_allowed(path);
}
constexpr std::array<std::string_view, 8> kSingletons{"world_info.json",
                                                      "aether_state.efs",
                                                      "chunks/region/world-manifest.json",
                                                      "chunks/region/plant-entities.json",
                                                      "chunks/region/creature-entities.json",
                                                      "chunks/region/ground-objects.json",
                                                      "chunks/region/field-pages.fpg",
                                                      "chunks/region/active-regions.arl"};
Error ValidateSingletons(const std::filesystem::path& root,
                         const SnapshotIndex& index,
                         const EditedSourceLimits& limits,
                         std::uint64_t prepared_generation,
                         const std::function<bool(std::string_view)>& protected_member,
                         std::uint64_t& file_bytes,
                         std::uint64_t& decode_bytes,
                         std::vector<SnapshotMember>& dependencies) {
    if (kSingletons.size() > limits.max_members)
        return Error::WorkLimit;
    // Preflight every sibling before format validators can follow ownership dependencies.
    for (const auto path : kSingletons) {
        if (!std::filesystem::exists(root / path))
            continue;
        const auto bytes = std::filesystem::file_size(root / path);
        if (bytes > limits.max_file_bytes - file_bytes ||
            bytes > limits.max_decode_bytes - decode_bytes ||
            bytes > limits.max_scratch_bytes / 256)
            return Error::ByteLimit;
        file_bytes += bytes;
        decode_bytes += bytes;
    }
    // The shared file hasher uses a fixed block; the ground-object reader has a fixed
    // input buffer. Ownership validators may retain several sibling projections together.
    std::uint64_t fixed_scratch = 1024 * 1024;
    if (std::filesystem::exists(WorldSaveService::ground_objects_path(root)))
        fixed_scratch += world::GroundObjectStore::kMaxFileBytes + 1;
    if (fixed_scratch > limits.max_scratch_bytes / 4 ||
        file_bytes > (limits.max_scratch_bytes / 2 - fixed_scratch) / 256)
        return Error::ByteLimit;
    for (const auto path : kSingletons) {
        MemberDigest actual;
        std::string diagnostic;
        if (!WorldSaveService::edited_source_member_digest(root, path, actual, diagnostic))
            return Error::InvalidRecord;
        const auto found = std::find_if(index.members.begin(),
                                        index.members.end(),
                                        [path](const auto& member) { return member.path == path; });
        const bool changed = (found == index.members.end() && actual.present) ||
                             (found != index.members.end() && !SameDigest(*found, actual));
        const bool is_protected = protected_member && protected_member(path);
        if (index.generation != 0 && changed && !is_protected)
            return Error::Unavailable;
        SnapshotMember dependency;
        dependency.path = path;
        dependency.present = actual.present;
        dependency.bytes = actual.present ? actual.bytes : 0;
        dependency.fnv64 = actual.present ? actual.fnv64 : 0;
        dependency.generation = found == index.members.end() ? 0 : found->generation;
        if (prepared_generation != 0 && (changed || (index.generation == 0 && actual.present) ||
                                         (found == index.members.end() && is_protected)))
            dependency.generation = prepared_generation;
        dependencies.push_back(std::move(dependency));
    }
    std::vector<std::string> diagnostics;
    if (!WorldSaveService::validate_authoritative_relationships(root, diagnostics, {}, true))
        return Error::InvalidRecord;
    return Error::None;
}
Error ReadJson(const std::filesystem::path& path,
               const EditedSourceLimits& limits,
               nlohmann::json& out,
               bool& present) {
    present = std::filesystem::exists(path);
    if (!present)
        return Error::None;
    const auto bytes = std::filesystem::file_size(path);
    if (bytes > limits.max_file_bytes || bytes > limits.max_decode_bytes ||
        bytes > limits.max_scratch_bytes / 128)
        return Error::ByteLimit;
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(bytes), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) ||
        input.peek() != std::char_traits<char>::eof())
        return Error::Unavailable;
    out = nlohmann::json::parse(text, nullptr, false);
    return out.is_object() ? Error::None : Error::InvalidRecord;
}
Error ValidateHeaders(const std::filesystem::path& root, const EditedSourceLimits& limits) {
    nlohmann::json value;
    bool present = false;
    auto error = ReadJson(root / "world_info.json", limits, value, present);
    if (error != Error::None)
        return error;
    if (present &&
        (!value.contains("container_version") || !value["container_version"].is_number_integer() ||
         value["container_version"] != WorldSaveService::kContainerVersion))
        return Error::InvalidRecord;
    if (present) {
        if (value.contains("snapshotFormat") &&
            (!value["snapshotFormat"].is_number_integer() || value["snapshotFormat"] < 0 ||
             value["snapshotFormat"] > 1))
            return Error::InvalidRecord;
        world::WorldClock clock;
        bool requires_regions = false;
        if (!WorldSaveService::read_clock_metadata(root, clock, requires_regions))
            return Error::InvalidRecord;
    }
    error = ReadJson(WorldSaveService::world_manifest_path(root), limits, value, present);
    if (error != Error::None)
        return error;
    if (present &&
        (value.value("schema", std::string{}) != "luminumbra.persistence.world_manifest.v1" ||
         value.value("container", std::string{}) != "LMR1" ||
         value.value("container_version", 0) != WorldSaveService::kContainerVersion))
        return Error::InvalidRecord;
    return Error::None;
}
Error PreflightRecovery(const std::filesystem::path& root, const EditedSourceLimits& limits) {
    if (limits.max_directory_entries > limits.max_scratch_bytes / 512 ||
        (journal_pending(root) && limits.max_scratch_bytes < 4u * 1024u * 1024u))
        return Error::ByteLimit;
    std::size_t visited = 0;
    if (std::filesystem::exists(root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (visited++ >= limits.max_directory_entries)
                return Error::WorkLimit;
            if (entry.is_symlink())
                return Error::Unavailable;
        }
    }
    const std::uint64_t fixed_scratch = 1024 * 1024 + visited * 512;
    if (fixed_scratch > limits.max_scratch_bytes / 2)
        return Error::ByteLimit;
    const auto payload_capacity = (limits.max_scratch_bytes / 2 - fixed_scratch) / 256;
    std::uint64_t admitted_bytes = 0;
    Error admission_error = Error::InvalidRecord;
    const bool admitted = admit_recovery_payloads(
        root,
        [&](const std::filesystem::path& path) {
            if (!std::filesystem::exists(path))
                return true;
            const auto bytes = std::filesystem::file_size(path);
            // Metadata JSON, undo contents, hash blocks and rollback ordering coexist.
            if (bytes > limits.max_file_bytes - admitted_bytes ||
                bytes > limits.max_decode_bytes - admitted_bytes ||
                bytes > payload_capacity - admitted_bytes) {
                admission_error = Error::ByteLimit;
                return false;
            }
            admitted_bytes += bytes;
            return true;
        },
        [&](std::size_t entries) {
            if (entries > limits.max_members || entries > limits.max_records ||
                entries > limits.max_scratch_bytes / (2 * 1024)) {
                admission_error = Error::WorkLimit;
                return false;
            }
            return true;
        });
    if (!admitted)
        return admission_error;
    return Error::None;
}
} // namespace

struct EditedSourceData {
    // Payload and descriptors die before their reservation, including all aliased handles.
    std::shared_ptr<Reservation> reservation;
    std::vector<Payload> payloads;
    std::vector<World::FarEditedLattice> lattices;
    std::vector<EditedSourceInventoryEntry> inventory;
    std::vector<EditedSourceWater> water;
    std::vector<SnapshotMember> dependencies;
    std::vector<EditedSourceTileSupport> selection;
    std::uint64_t used_bytes = 0;
};

namespace {
Error AllocateData(const EditedSourceSelection& selection,
                   const EditedSourceLimits& limits,
                   std::shared_ptr<EditedSourceData>& out) {
    if (!limits.memory)
        return Error::ByteLimit;
    const auto per_record = sizeof(Payload) + sizeof(World::FarEditedLattice) +
                            sizeof(EditedSourceInventoryEntry) + sizeof(EditedSourceWater);
    // Dependency copies in the publication token also belong to this reservation.
    const auto per_member = 2 * (sizeof(SnapshotMember) + kPathBytes);
    std::uint64_t controls = sizeof(EditedSourceData) + sizeof(EditedSourceImage) +
                             sizeof(EditedSourcePublicationToken) + 1024;
    if (controls > limits.max_image_bytes ||
        limits.max_records > (limits.max_image_bytes - controls) / per_record)
        return Error::ByteLimit;
    controls += limits.max_records * per_record;
    if (limits.max_members > (limits.max_image_bytes - controls) / per_member)
        return Error::ByteLimit;
    controls += limits.max_members * per_member;
    if (selection.tiles.size() >
        (limits.max_image_bytes - controls) / sizeof(EditedSourceTileSupport))
        return Error::ByteLimit;
    controls += selection.tiles.size() * sizeof(EditedSourceTileSupport);
    World::FarVolumeMemoryLease admission(*limits.memory);
    if (!admission.TryCharge(limits.max_image_bytes))
        return Error::ByteLimit;
    auto data = std::make_shared<EditedSourceData>();
    data->reservation = std::make_shared<Reservation>(limits.memory);
    data->reservation->lease = std::move(admission);
    data->used_bytes = controls;
    data->payloads.reserve(limits.max_records);
    data->lattices.reserve(limits.max_records);
    data->inventory.reserve(limits.max_records);
    data->water.reserve(limits.max_records);
    data->dependencies.reserve(limits.max_members);
    data->selection.assign(selection.tiles.begin(), selection.tiles.end());
    out = std::move(data);
    return Error::None;
}
struct CaptureWork {
    std::uint64_t samples = 0, selection_probes = 0, boundary_probes = 0;
};
Error AppendChunk(EditedSourceData& data,
                  CaptureWork& work,
                  const Chunk& chunk,
                  std::size_t member_index,
                  std::uint64_t generation,
                  std::uint64_t digest,
                  const EditedSourceSelection& selection,
                  const EditedSourceLimits& limits) {
    if (selection.tiles.size() > (limits.max_selection_probes - work.selection_probes) / 7)
        return Error::WorkLimit;
    work.selection_probes += selection.tiles.size() * 7;
    const auto footprint = ChunkFootprint(chunk);
    const bool relevant = Selected(footprint, selection);
    auto payload_limit = std::min(limits.max_decode_bytes, limits.max_scratch_bytes / 2);
    if (relevant)
        payload_limit = std::min(payload_limit, limits.max_image_bytes - data.used_bytes);
    std::uint64_t bytes = 0;
    if (!PayloadBytes(chunk, payload_limit, bytes))
        return Error::ByteLimit;
    if (kSamples > limits.max_samples - work.samples)
        return Error::WorkLimit;
    work.samples += kSamples;
    // Validate every authoritative record, including values a coarse tile would never sample.
    auto error = ValidateChunk(chunk);
    if (error != Error::None)
        return error;
    if (!relevant)
        return Error::None;
    const auto lo = Axes(footprint.min);
    const auto hi = Axes(footprint.max);
    for (std::size_t axis = 0; axis < lo.size(); ++axis) {
        if (lo[axis] <= -kCoordinateLimit || hi[axis] >= kCoordinateLimit)
            return Error::InvalidRecord;
    }
    if (data.payloads.size() >= limits.max_records)
        return Error::WorkLimit;
    data.used_bytes += bytes;
    data.payloads.push_back(Payload{chunk.sdf_data, chunk.material_data});
    const auto& payload = data.payloads.back();
    World::FarEditedLattice lattice;
    lattice.origin = footprint.min;
    lattice.spacing_meters = 1;
    lattice.side = {kSide, kSide, kSide};
    lattice.density = payload.density;
    lattice.material = payload.material;
    lattice.member_generation = generation;
    data.lattices.push_back(lattice);
    data.inventory.push_back({chunk.get_id(), footprint, member_index, generation, digest});
    data.water.push_back(CopyWater(chunk));
    return Error::None;
}
std::size_t NodeIndex(const Position& point, const World::FarEditedLattice& lattice) {
    return static_cast<std::size_t>(point.x - lattice.origin.x) +
           kSide * (static_cast<std::size_t>(point.y - lattice.origin.y) +
                    kSide * static_cast<std::size_t>(point.z - lattice.origin.z));
}
bool EqualNode(const Position& point,
               const World::FarEditedLattice& a,
               const World::FarEditedLattice& b) {
    const auto ia = NodeIndex(point, a);
    const auto ib = NodeIndex(point, b);
    const auto ma = a.material.empty() ? 0 : a.material[ia];
    const auto mb = b.material.empty() ? 0 : b.material[ib];
    return a.density[ia] == b.density[ib] && ma == mb;
}
Error ValidateBoundaries(const EditedSourceData& data,
                         const EditedSourceLimits& limits,
                         CaptureWork& work) {
    auto& probes = work.boundary_probes;
    for (std::size_t i = 0; i < data.lattices.size(); ++i) {
        for (std::size_t j = i + 1; j < data.lattices.size(); ++j) {
            if (probes == limits.max_boundary_probes)
                return Error::WorkLimit;
            ++probes;
            const auto& a = data.inventory[i].footprint;
            const auto& b = data.inventory[j].footprint;
            if (!Intersects(a, b))
                continue;
            const Position lo{
                std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)};
            const Position hi{
                std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)};
            const auto nodes = static_cast<std::uint64_t>(hi.x - lo.x + 1) *
                               static_cast<std::uint64_t>(hi.y - lo.y + 1) *
                               static_cast<std::uint64_t>(hi.z - lo.z + 1);
            if (nodes > limits.max_boundary_probes - probes)
                return Error::WorkLimit;
            probes += nodes;
            for (auto z = lo.z; z <= hi.z; ++z) {
                for (auto y = lo.y; y <= hi.y; ++y) {
                    for (auto x = lo.x; x <= hi.x; ++x) {
                        if (!EqualNode({x, y, z}, data.lattices[i], data.lattices[j]))
                            return Error::BoundaryConflict;
                    }
                }
            }
        }
    }
    return Error::None;
}
} // namespace

class EditedSourceCaptureAccess {
public:
    static Error Capture(const std::filesystem::path& root,
                         std::uint64_t expected,
                         SaveTransaction* transaction,
                         const EditedSourceSelection& selection,
                         const EditedSourceLimits& limits,
                         std::shared_ptr<const EditedSourceImage>& out);
    static Error Publish(const std::shared_ptr<const EditedSourceImage>& prepared,
                         std::shared_ptr<const EditedSourceImage>& out);
    static Error Preview(const EditedSourcePreviewProducer& producer,
                         const EditedSourceSelection& selection,
                         const EditedSourceLimits& limits,
                         std::shared_ptr<const EditedSourcePreviewLayer>& live,
                         std::shared_ptr<const EditedSourcePreviewLayer>& parked);

private:
    static Error Load(const std::filesystem::path& root,
                      const SnapshotIndex& index,
                      SaveTransaction* transaction,
                      const EditedSourceSelection& selection,
                      const EditedSourceLimits& limits,
                      EditedSourceData& data);
};

Error EditedSourceCaptureAccess::Load(const std::filesystem::path& root,
                                      const SnapshotIndex& index,
                                      SaveTransaction* transaction,
                                      const EditedSourceSelection& selection,
                                      const EditedSourceLimits& limits,
                                      EditedSourceData& data) {
    CaptureWork work;
    std::vector<std::string> regions;
    regions.reserve(limits.max_members);
    for (const auto& member : index.members) {
        if (member.generation > index.generation ||
            (RegionPath(member.path) && member.cls != MemberClass::Authoritative))
            return Error::GenerationMismatch;
        if (!RegionPath(member.path))
            continue;
        if (regions.size() == limits.max_members)
            return Error::WorkLimit;
        regions.push_back(member.path);
    }
    const auto directory = WorldSaveService::region_directory(root);
    if (std::filesystem::exists(directory)) {
        std::size_t visited = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (visited++ >= limits.max_directory_entries)
                return Error::WorkLimit;
            const auto path = "chunks/region/" + entry.path().filename().string();
            if (!RegionPath(path)) {
                if (entry.path().extension() == ".lmr")
                    return Error::InvalidRecord;
                continue;
            }
            if (!entry.is_regular_file())
                return Error::Unavailable;
            if (std::find(regions.begin(), regions.end(), path) != regions.end())
                continue;
            if (regions.size() == limits.max_members)
                return Error::WorkLimit;
            regions.push_back(path);
        }
    }
    std::sort(regions.begin(), regions.end());
    std::uint64_t decoded_total = 0, file_total = 0;
    const auto singleton_error = ValidateSingletons(
        root,
        index,
        limits,
        transaction ? transaction->new_generation() : 0,
        [transaction](std::string_view path) {
            return transaction && transaction->capture_member_protected(path);
        },
        file_total,
        decoded_total,
        data.dependencies);
    if (singleton_error != Error::None)
        return singleton_error;
    if (regions.size() > limits.max_members - data.dependencies.size())
        return Error::WorkLimit;
    std::size_t record_total = 0;
    for (const auto& path : regions) {
        const auto found = std::lower_bound(
            index.members.begin(),
            index.members.end(),
            path,
            [](const auto& member, const auto& name) { return member.path < name; });
        const SnapshotMember* previous =
            found != index.members.end() && found->path == path ? &*found : nullptr;
        const bool protected_member = transaction && transaction->capture_member_protected(path);
        MemberDigest actual;
        std::uint64_t decoded = 0;
        std::size_t read = 0;
        const auto first_record = data.lattices.size();
        const auto member_index = data.dependencies.size();
        if (std::filesystem::exists(root / path)) {
            const auto size = std::filesystem::file_size(root / path);
            if (size > limits.max_file_bytes - file_total)
                return Error::ByteLimit;
            file_total += size;
            Error callback_error = Error::None;
            auto admitted_samples = limits.max_samples - work.samples;
            if (!selection.tiles.empty()) {
                const auto admitted_records =
                    std::min(admitted_samples / kSamples,
                             (limits.max_selection_probes - work.selection_probes) / 7 /
                                 selection.tiles.size());
                admitted_samples = admitted_records * kSamples;
            }
            const WorldSaveService::EditedRegionReadLimits read_limits{
                limits.max_file_bytes - (file_total - size),
                limits.max_decode_bytes - decoded_total,
                limits.max_scratch_bytes / 2,
                limits.max_records - record_total,
                admitted_samples};
            if (!WorldSaveService::read_edited_source_region(
                    root / path,
                    read_limits,
                    index.generation != 0,
                    [&](const Chunk& chunk, std::uint64_t digest) {
                        callback_error = AppendChunk(
                            data, work, chunk, member_index, 0, digest, selection, limits);
                        return callback_error == Error::None;
                    },
                    actual,
                    decoded,
                    read))
                return callback_error == Error::None ? Error::InvalidRecord : callback_error;
        }
        decoded_total += decoded;
        record_total += read;
        if (index.generation != 0 && !protected_member &&
            ((!previous && actual.present) || (previous && !SameDigest(*previous, actual))))
            return Error::Unavailable;
        SnapshotMember dependency;
        dependency.path = path;
        dependency.present = actual.present;
        dependency.bytes = actual.present ? actual.bytes : 0;
        dependency.fnv64 = actual.present ? actual.fnv64 : 0;
        dependency.generation = previous ? previous->generation : 0;
        if (transaction && (!previous || !SameDigest(*previous, actual))) {
            if (index.generation != 0 && !protected_member)
                return Error::Unavailable;
            dependency.generation = transaction->new_generation();
        }
        // First adoption stamps present members at the new generation, including unchanged ones.
        if (transaction && index.generation == 0 && actual.present)
            dependency.generation = transaction->new_generation();
        for (std::size_t record = first_record; record < data.lattices.size(); ++record) {
            data.lattices[record].member_generation = dependency.generation;
            data.inventory[record].member_generation = dependency.generation;
        }
        data.dependencies.push_back(std::move(dependency));
    }
    return ValidateBoundaries(data, limits, work);
}

Error EditedSourceCaptureAccess::Capture(const std::filesystem::path& root,
                                         std::uint64_t expected,
                                         SaveTransaction* transaction,
                                         const EditedSourceSelection& selection,
                                         const EditedSourceLimits& limits,
                                         std::shared_ptr<const EditedSourceImage>& out) {
    try {
        const auto selected = ValidateSelection(selection, limits);
        if (selected != Error::None)
            return selected;
        if (!limits.memory || limits.max_scratch_bytes == 0)
            return Error::ByteLimit;
        World::FarVolumeMemoryLease scratch(*limits.memory);
        if (!scratch.TryCharge(limits.max_scratch_bytes))
            return Error::ByteLimit;
        if (transaction) {
            if (!transaction->capture_open_on_owner_thread())
                return Error::Ineligible;
            if (transaction->new_generation() == 0)
                return Error::GenerationMismatch;
        } else {
            if (SaveTransaction::active_for(root))
                return Error::Ineligible;
            const auto preflight = PreflightRecovery(root, limits);
            if (preflight != Error::None)
                return preflight;
            if (recover_pending_journal(root, nullptr) == RecoveryOutcome::Failed)
                return Error::Unavailable;
        }
        auto error = ValidateHeaders(root, limits);
        if (error != Error::None)
            return error;
        SnapshotIndex index;
        if (transaction) {
            if (transaction->m_previous.members.size() > limits.max_members ||
                transaction->m_previous.members.size() >
                    limits.max_scratch_bytes / (4 * (sizeof(SnapshotMember) + kPathBytes)) ||
                transaction->m_previous.anchors.size() >
                    limits.max_scratch_bytes / (4 * sizeof(std::array<float, 3>)))
                return Error::WorkLimit;
            index = transaction->m_previous;
        } else {
            const auto path = snapshot_index_path(root);
            if (std::filesystem::exists(path)) {
                const auto bytes = std::filesystem::file_size(path);
                if (bytes > limits.max_file_bytes || bytes > limits.max_decode_bytes ||
                    bytes > limits.max_scratch_bytes / 256)
                    return Error::ByteLimit;
            }
            if (!read_snapshot_index(root, index, nullptr))
                return Error::InvalidRecord;
            if (index.generation != expected)
                return Error::GenerationMismatch;
        }
        if (index.members.size() > limits.max_members)
            return Error::WorkLimit;
        if (limits.max_members >
            limits.max_scratch_bytes / (4 * (sizeof(SnapshotMember) + kPathBytes)))
            return Error::ByteLimit;
        std::shared_ptr<EditedSourceData> data;
        error = AllocateData(selection, limits, data);
        if (error != Error::None)
            return error;
        error = Load(root, index, transaction, selection, limits, *data);
        if (error != Error::None)
            return error;
        std::shared_ptr<EditedSourcePublicationToken> token;
        if (transaction) {
            auto slot = std::find_if(transaction->m_edited_sources.begin(),
                                     transaction->m_edited_sources.end(),
                                     [](const auto& weak) { return weak.expired(); });
            if (slot == transaction->m_edited_sources.end())
                return Error::WorkLimit;
            token.reset(new EditedSourcePublicationToken());
            token->m_dependencies = data->dependencies;
            token->m_retention = data->reservation;
            *slot = token;
        }
        World::FarEditedImageIdentity identity;
        identity.generation = transaction ? transaction->new_generation() : index.generation;
        identity.state = transaction ? World::FarEditedImageState::Prepared
                                     : World::FarEditedImageState::Committed;
        auto result = std::shared_ptr<const EditedSourceImage>(
            new EditedSourceImage(std::move(data), identity, std::move(token)));
        out = std::move(result);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::exception&) {
        return Error::InvalidRecord;
    }
}

Error EditedSourceCaptureAccess::Publish(const std::shared_ptr<const EditedSourceImage>& prepared,
                                         std::shared_ptr<const EditedSourceImage>& out) {
    if (!prepared || prepared->m_identity.state != World::FarEditedImageState::Prepared ||
        !prepared->m_publication || !prepared->m_publication->committed() ||
        prepared->m_publication->m_owner_thread != std::this_thread::get_id())
        return Error::Ineligible;
    try {
        const auto memory = prepared->m_data->reservation->memory;
        World::FarVolumeMemoryLease admission(*memory);
        if (!admission.TryCharge(512))
            return Error::ByteLimit;
        auto reservation = std::make_shared<Reservation>(memory);
        reservation->lease = std::move(admission);
        auto identity = prepared->m_identity;
        identity.state = World::FarEditedImageState::Committed;
        auto result = std::shared_ptr<const EditedSourceImage>(new EditedSourceImage(
            prepared->m_data, identity, prepared->m_publication, std::move(reservation)));
        out = std::move(result);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    }
}

Error EditedSourceCaptureAccess::Preview(const EditedSourcePreviewProducer& producer,
                                         const EditedSourceSelection& selection,
                                         const EditedSourceLimits& limits,
                                         std::shared_ptr<const EditedSourcePreviewLayer>& live,
                                         std::shared_ptr<const EditedSourcePreviewLayer>& parked) {
    try {
        auto error = ValidateSelection(selection, limits);
        if (error != Error::None)
            return error;
        if (!producer || !limits.memory)
            return Error::Unavailable;
        if (limits.max_records > limits.max_scratch_bytes / 64)
            return Error::ByteLimit;
        World::FarVolumeMemoryLease scratch(*limits.memory);
        if (!scratch.TryCharge(limits.max_scratch_bytes))
            return Error::ByteLimit;
        std::shared_ptr<EditedSourceData> live_data, parked_data;
        error = AllocateData(selection, limits, live_data);
        if (error != Error::None)
            return error;
        error = AllocateData(selection, limits, parked_data);
        if (error != Error::None)
            return error;
        CaptureWork work;
        std::size_t visits = 0;
        Error callback_error = Error::None;
        const bool produced = producer(limits.max_records, [&](const Chunk& chunk, bool is_parked) {
            if (visits++ >= limits.max_records) {
                callback_error = Error::WorkLimit;
                return false;
            }
            if (!chunk.needs_authority())
                return true;
            auto& data = is_parked ? *parked_data : *live_data;
            callback_error = AppendChunk(data, work, chunk, 0, 0, 0, selection, limits);
            return callback_error == Error::None;
        });
        if (!produced)
            return callback_error == Error::None ? Error::Unavailable : callback_error;
        error = ValidateBoundaries(*live_data, limits, work);
        if (error != Error::None)
            return error;
        error = ValidateBoundaries(*parked_data, limits, work);
        if (error != Error::None)
            return error;
        auto live_result = std::shared_ptr<const EditedSourcePreviewLayer>(
            new EditedSourcePreviewLayer(std::move(live_data)));
        auto parked_result = std::shared_ptr<const EditedSourcePreviewLayer>(
            new EditedSourcePreviewLayer(std::move(parked_data)));
        live = std::move(live_result);
        parked = std::move(parked_result);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::exception&) {
        return Error::InvalidRecord;
    }
}

EditedSourceImage::EditedSourceImage(std::shared_ptr<const EditedSourceData> data,
                                     World::FarEditedImageIdentity identity,
                                     std::shared_ptr<EditedSourcePublicationToken> publication,
                                     std::shared_ptr<const void> handle_reservation)
    : m_handleReservation(std::move(handle_reservation))
    , m_data(std::move(data))
    , m_identity(identity)
    , m_publication(std::move(publication)) {}
World::FarEditedImageIdentity EditedSourceImage::Identity() const {
    return m_identity;
}
std::span<const World::FarEditedLattice> EditedSourceImage::Lattices() const {
    return m_data->lattices;
}
std::span<const EditedSourceInventoryEntry> EditedSourceImage::Inventory() const {
    return m_data->inventory;
}
std::span<const SnapshotMember> EditedSourceImage::Dependencies() const {
    return m_data->dependencies;
}
std::span<const EditedSourceWater> EditedSourceImage::Water() const {
    return m_data->water;
}
std::span<const EditedSourceTileSupport> EditedSourceImage::Selection() const {
    return m_data->selection;
}
bool EditedSourceImage::LegacyAuthority() const {
    return m_identity.generation == 0;
}
bool EditedSourceImage::RendererEligible() const {
    return m_identity.state == World::FarEditedImageState::Committed &&
           (!m_publication || m_publication->committed());
}
std::shared_ptr<const SnapshotCommitReceipt> EditedSourceImage::PublicationReceipt() const {
    return m_publication ? m_publication->receipt() : nullptr;
}
std::uint64_t EditedSourceImage::ReservedBytes() const {
    return m_data->reservation->lease.bytes() + (m_handleReservation ? 512 : 0);
}
EditedSourcePreviewLayer::EditedSourcePreviewLayer(std::shared_ptr<const EditedSourceData> data)
    : m_data(std::move(data)) {}
std::span<const World::FarEditedLattice> EditedSourcePreviewLayer::Lattices() const {
    return m_data->lattices;
}
std::span<const EditedSourceInventoryEntry> EditedSourcePreviewLayer::Inventory() const {
    return m_data->inventory;
}
std::span<const EditedSourceWater> EditedSourcePreviewLayer::Water() const {
    return m_data->water;
}

EditedSourceError CaptureCommittedEditedSource(const std::filesystem::path& save_root,
                                               std::uint64_t expected_generation,
                                               const EditedSourceSelection& selection,
                                               const EditedSourceLimits& limits,
                                               std::shared_ptr<const EditedSourceImage>& out) {
    return EditedSourceCaptureAccess::Capture(
        save_root, expected_generation, nullptr, selection, limits, out);
}
EditedSourceError CapturePreparedEditedSource(SaveTransaction& transaction,
                                              const EditedSourceSelection& selection,
                                              const EditedSourceLimits& limits,
                                              std::shared_ptr<const EditedSourceImage>& out) {
    return EditedSourceCaptureAccess::Capture(
        transaction.save_dir(), 0, &transaction, selection, limits, out);
}
EditedSourceError
PublishCommittedEditedSource(const std::shared_ptr<const EditedSourceImage>& prepared,
                             std::shared_ptr<const EditedSourceImage>& out) {
    return EditedSourceCaptureAccess::Publish(prepared, out);
}
EditedSourceError
CaptureEditedSourcePreview(const EditedSourcePreviewProducer& producer,
                           const EditedSourceSelection& selection,
                           const EditedSourceLimits& limits,
                           std::shared_ptr<const EditedSourcePreviewLayer>& live,
                           std::shared_ptr<const EditedSourcePreviewLayer>& parked) {
    return EditedSourceCaptureAccess::Preview(producer, selection, limits, live, parked);
}
} // namespace Luminumbra::Persistence
