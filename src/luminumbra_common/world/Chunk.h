#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include "core/WaterSeedInputs.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Luminumbra {

enum class ChunkState : u8 {
    Unloaded,
    Loading,
    Idle,
    Meshing,
    Ready,
    Unloading
};

// Persisted or edited SDFs remain authoritative after a save clears the
// transient dirty bit.  Far-LOD capture uses this explicit provenance.
enum class ChunkSdfProvenance : u8 {
    GeneratedCurrentParams,
    LoadedOrEdited,
};

struct VoxelVertex {
    Vec3 position;
    Vec3 normal;
    u32 material_id;
};

class Chunk {
public:
    static constexpr i32 kPackedMinXz = -(1 << 20);
    static constexpr i32 kPackedMaxXz = (1 << 20) - 1;
    static constexpr i32 kPackedMinY = -(1 << 21);
    static constexpr i32 kPackedMaxY = (1 << 21) - 1;

    Chunk(const IVec3& coords);
    const IVec3& get_coords() const {
        return m_coords;
    }
    ChunkID get_id() const {
        return m_id;
    }

    // Thread-safe state management
    ChunkState get_state() const {
        LUMIN_PROFILE_LOCK(
            std::lock_guard<std::mutex>, lock, m_state_mutex, "Chunk state lock wait");
        return m_state;
    }

    void set_state(ChunkState new_state) {
        LUMIN_PROFILE_LOCK(
            std::lock_guard<std::mutex>, lock, m_state_mutex, "Chunk state lock wait");
        m_state = new_state;
    }

    // --- Voxel edit tracking ---
    // True when sdf/heightmap voxel data was mutated AFTER generation and has
    // not been persisted yet. Generation, loading, and meshing leave the flag
    // clear; runtime voxel edits must call mark_voxel_data_dirty, and a
    // successful save clears it again.
    void mark_voxel_data_dirty() {
        advance_water_seed_edit_generation();
        m_sdf_provenance.store(static_cast<u8>(ChunkSdfProvenance::LoadedOrEdited),
                               std::memory_order_release);
        u32 revision = m_voxel_revision.load(std::memory_order_acquire);
        while (revision != std::numeric_limits<u32>::max() &&
               !m_voxel_revision.compare_exchange_weak(
                   revision, revision + 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        m_voxel_data_dirty.store(true, std::memory_order_release);
    }
    void clear_voxel_data_dirty() {
        m_voxel_data_dirty.store(false, std::memory_order_release);
    }
    bool is_voxel_data_dirty() const {
        return m_voxel_data_dirty.load(std::memory_order_acquire);
    }
    void mark_sdf_generated_current_params() {
        if (has_water_authority()) {
            // Water divergence is sticky authority: a generation/promotion backfill of the
            // terrain must not make the chunk regenerable again.
            m_sdf_provenance.store(static_cast<u8>(ChunkSdfProvenance::LoadedOrEdited),
                                   std::memory_order_release);
            u32 expected = 0u;
            m_voxel_revision.compare_exchange_strong(
                expected, 1u, std::memory_order_acq_rel, std::memory_order_acquire);
            // Leave the voxel-dirty bit untouched: it may carry an unsaved water obligation.
            return;
        }
        m_sdf_provenance.store(static_cast<u8>(ChunkSdfProvenance::GeneratedCurrentParams),
                               std::memory_order_release);
        m_voxel_revision.store(0u, std::memory_order_release);
        m_voxel_data_dirty.store(false, std::memory_order_release);
    }
    // Post-generation acknowledgement: a promoted/backfilled terrain is pure generation output,
    // except on water-authoritative chunks where the dirty bit may carry unsaved water state.
    void clear_voxel_data_dirty_after_generation() {
        if (!has_water_authority()) {
            clear_voxel_data_dirty();
        }
    }
    void mark_sdf_loaded_or_edited() {
        advance_water_seed_edit_generation();
        m_sdf_provenance.store(static_cast<u8>(ChunkSdfProvenance::LoadedOrEdited),
                               std::memory_order_release);
        u32 expected = 0u;
        m_voxel_revision.compare_exchange_strong(
            expected, 1u, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    ChunkSdfProvenance sdf_provenance() const {
        return static_cast<ChunkSdfProvenance>(m_sdf_provenance.load(std::memory_order_acquire));
    }
    u32 voxel_revision() const {
        return m_voxel_revision.load(std::memory_order_acquire);
    }

    // --- Durable water state (S3 durable residency) ---
    // Water fields (depth/flux/bed/sleep bookkeeping) are persisted evolution state. Every writer
    // that changes them calls mark_water_state_dirty(). The bit is flag independent and
    // unobservable unless SHIELD_WorldSystem::sync_water_authority() consumes it (only called when
    // sim.active_regions is on).
    void mark_water_state_dirty() {
        u32 revision = m_water_revision.load(std::memory_order_acquire);
        while (revision != std::numeric_limits<u32>::max() &&
               !m_water_revision.compare_exchange_weak(
                   revision, revision + 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        m_water_state_dirty.store(true, std::memory_order_release);
    }
    bool is_water_state_dirty() const {
        return m_water_state_dirty.load(std::memory_order_acquire);
    }
    void clear_water_state_dirty() {
        m_water_state_dirty.store(false, std::memory_order_release);
    }
    u32 water_revision() const {
        return m_water_revision.load(std::memory_order_acquire);
    }
    // Sticky: set once the chunk's water state has diverged from worldgen initialisation.
    void set_water_authority() {
        m_water_authority.store(true, std::memory_order_release);
    }
    bool has_water_authority() const {
        return m_water_authority.load(std::memory_order_acquire);
    }
    // True when the chunk cannot be regenerated byte-identically from worldgen.
    bool needs_authority() const {
        return sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited || has_water_authority() ||
               is_water_state_dirty();
    }
    // Re-arms the save-dirty bit WITHOUT touching provenance or the voxel revision (used to
    // transfer water divergence into the existing save selection and to retry after a failed save).
    void mark_dirty_for_save() {
        m_voxel_data_dirty.store(true, std::memory_order_release);
    }
    // Approximate heap footprint: sum of vector capacities * element size, including water and
    // mesh buffers. Used for the parked-bytes budget.
    std::size_t estimated_heap_bytes() const {
        std::size_t bytes = 0;
        bytes += sdf_data.capacity() * sizeof(f32);
        bytes += heightmap_data.capacity() * sizeof(f32);
        bytes += material_data.capacity() * sizeof(u8);
        bytes += mesh_vertices.capacity() * sizeof(VoxelVertex);
        bytes += mesh_indices.capacity() * sizeof(u32);
        bytes += water_mesh_vertices.capacity() * sizeof(VoxelVertex);
        bytes += water_mesh_indices.capacity() * sizeof(u32);
        bytes += pending_mesh_vertices.capacity() * sizeof(VoxelVertex);
        bytes += pending_mesh_indices.capacity() * sizeof(u32);
        bytes += pending_water_mesh_vertices.capacity() * sizeof(VoxelVertex);
        bytes += pending_water_mesh_indices.capacity() * sizeof(u32);
        bytes += pending_sdf_data.capacity() * sizeof(f32);
        bytes += pending_heightmap_data.capacity() * sizeof(f32);
        bytes += pending_material_data.capacity() * sizeof(u8);
        bytes += water_level_data.capacity() * sizeof(f32);
        bytes += water_flow_data.capacity() * sizeof(Vec2);
        bytes += water_sim_terrain_height.capacity() * sizeof(f32);
        bytes += water_rest_level.capacity() * sizeof(f32);
        bytes += water_depth_mm.capacity() * sizeof(std::int32_t);
        bytes += water_bed_mm.capacity() * sizeof(std::int32_t);
        bytes += water_edge_flux.capacity() * sizeof(std::int32_t);
        bytes += water_src_mm.capacity() * sizeof(std::int32_t);
        if (m_water_seed_inputs) {
            bytes += sizeof(HighWaterSeedInputs);
        }
        return bytes;
    }
    // Sim-truth footprint only (what a parked chunk keeps after normalize_for_park()).
    std::size_t estimated_sim_bytes() const {
        std::size_t bytes = 0;
        bytes += sdf_data.capacity() * sizeof(f32);
        bytes += heightmap_data.capacity() * sizeof(f32);
        bytes += material_data.capacity() * sizeof(u8);
        bytes += water_level_data.capacity() * sizeof(f32);
        bytes += water_flow_data.capacity() * sizeof(Vec2);
        bytes += water_sim_terrain_height.capacity() * sizeof(f32);
        bytes += water_rest_level.capacity() * sizeof(f32);
        bytes += water_depth_mm.capacity() * sizeof(std::int32_t);
        bytes += water_bed_mm.capacity() * sizeof(std::int32_t);
        bytes += water_edge_flux.capacity() * sizeof(std::int32_t);
        bytes += water_src_mm.capacity() * sizeof(std::int32_t);
        return bytes;
    }
    // Drops every render-side buffer and transient pipeline marker so a quiescent chunk can sit in
    // the parked store. Sim truth (sdf/heightmap/material/water state) is untouched. The caller
    // guarantees no job owns the chunk.
    void normalize_for_park() {
        // Derived seed samples are dispensable; parked storage retains only simulation truth.
        stage_water_seed_inputs({});
        has_collision.store(false, std::memory_order_release);
        current_lod.store(-1, std::memory_order_release);
        pending_lod.store(-1, std::memory_order_release);
        applied_transition_faces.store(0, std::memory_order_release);
        pending_mesh_ready.store(false, std::memory_order_release);
        pending_mesh_failed.store(false, std::memory_order_release);
        pending_promotion_ready.store(false, std::memory_order_release);
        pending_promotion_failed.store(false, std::memory_order_release);
        pending_generation_ready.store(false, std::memory_order_release);
        water_mesh_generated.store(false, std::memory_order_release);
        std::vector<VoxelVertex>().swap(mesh_vertices);
        std::vector<u32>().swap(mesh_indices);
        std::vector<VoxelVertex>().swap(water_mesh_vertices);
        std::vector<u32>().swap(water_mesh_indices);
        std::vector<VoxelVertex>().swap(pending_mesh_vertices);
        std::vector<u32>().swap(pending_mesh_indices);
        std::vector<VoxelVertex>().swap(pending_water_mesh_vertices);
        std::vector<u32>().swap(pending_water_mesh_indices);
        std::vector<f32>().swap(pending_sdf_data);
        std::vector<f32>().swap(pending_heightmap_data);
        std::vector<u8>().swap(pending_material_data);
        set_state(ChunkState::Idle);
    }

    // Unlike voxel_revision, this derived-input key never resets on regeneration:
    // an edit followed by regeneration must not revive a retired seed payload.
    [[nodiscard]] std::uint64_t water_seed_edit_generation() const {
        return m_water_seed_edit_generation.load(std::memory_order_acquire);
    }
    void stage_water_seed_inputs(std::shared_ptr<HighWaterSeedInputs> inputs) {
        m_water_seed_inputs = std::move(inputs);
    }
    std::shared_ptr<HighWaterSeedInputs> take_water_seed_inputs() {
        return std::exchange(m_water_seed_inputs, {});
    }
    // Const diagnostic of the pending acceleration data; never simulation state.
    [[nodiscard]] const HighWaterSeedInputs* water_seed_inputs() const {
        return m_water_seed_inputs.get();
    }

    // --- Voxel & SDF Data ---
    std::vector<f32> sdf_data;
    std::vector<f32> heightmap_data;

    //  per-voxel material channel. Parallel to sdf_data, same index formula
    // (x + y*(CHUNK_SIZE_X+1) + z*(CHUNK_SIZE_X+1)*(CHUNK_SIZE_Y+1)). LAZILY
    // allocated: stays EMPTY for chunks with no authored structure voxels so
    // pristine/structures-off worlds serialize and hash byte-identically. Only
    // StampStructuresIntoChunk allocates it (assign(padded_volume, 0) where
    // 0 = MaterialType::Air sentinel = "no authored material; classify
    // analytically"). Guard.empty before every read. Far-LOD (step>1) chunks
    // carry no structure material (documented gap) and never allocate this.
    std::vector<u8> material_data;

    // --- Render Data ---
    std::vector<VoxelVertex> mesh_vertices;
    std::vector<u32> mesh_indices;
    std::vector<VoxelVertex> water_mesh_vertices;
    std::vector<u32> water_mesh_indices;

    std::vector<VoxelVertex> pending_mesh_vertices;
    std::vector<u32> pending_mesh_indices;
    std::vector<VoxelVertex> pending_water_mesh_vertices;
    std::vector<u32> pending_water_mesh_indices;

    //  ( step 1): voxel data produced inside a PROMOTION
    // generation job (LOD0 promotion of a chunk that was generated
    // surface-band-only), staged for publication on the main thread by
    // process_completed_promotion_jobs — strictly BEFORE the render-mesh
    // (stage B) dispatch, so sim truth never waits on meshing and sdf_data is
    // never written off the main thread. The meshing lane neither writes nor
    // clears these. The previous coarse mesh stays renderable while pending.
    std::vector<f32> pending_sdf_data;
    std::vector<f32> pending_heightmap_data;
    // the promotion lane stamps structure materials into the scratch
    // chunk and stages them here for main-thread publication alongside
    // pending_sdf_data (else a promoted chunk loses its structure materials
    // -> run != replay). Empty when the promoted chunk has no structure
    // voxels (lazy alloc preserved across the lane).
    std::vector<u8> pending_material_data;

    std::atomic<bool> has_collision{false};
    std::atomic<int> current_lod{-1};
    std::atomic<int> pending_lod{-1};
    // Bitmask of MarchingCubes::TerrainTransitionFace skirts baked into the
    // current mesh_vertices. Lets the streaming update detect coarse chunks
    // whose finer neighbors arrived AFTER this chunk was meshed (persistent
    // LOD seam cracks) without scanning mesh vertices per frame.
    std::atomic<u8> applied_transition_faces{0};
    std::atomic<bool> pending_mesh_ready{false};
    std::atomic<bool> pending_mesh_failed{false};
    //  ( step 1): completion signals for the sim-truth
    // PROMOTION lane. A promotion generation job stages the full LOD0 voxel
    // field into pending_sdf/heightmap/material_data and raises _ready; the
    // main thread publishes it in process_completed_promotion_jobs — before
    // and independently of any render-mesh publish.
    std::atomic<bool> pending_promotion_ready{false};
    std::atomic<bool> pending_promotion_failed{false};
    //   (activation queue): generation-completion signal. The generation
    // job writes this chunk's voxel data and raises the flag; the MAIN thread
    // performs the Loading→Idle flip (publish_completed_generation_jobs) — the
    // chunk state machine is main-thread-owned, so lifecycle is a pure
    // function of main-thread events (the activation queue's requirement),
    // never a worker-timing side effect.
    std::atomic<bool> pending_generation_ready{false};
    std::atomic<u32> mesh_version{0};
    std::atomic<u32> water_mesh_version{0};

    // --- Water Simulation Data ---
    std::vector<f32> water_level_data;
    std::vector<Vec2> water_flow_data;
    std::vector<f32> water_sim_terrain_height;
    // Worldgen rest level per cell (WaterLevelAt: lake surface in basins, sea level
    // elsewhere). The flow sim is clamped to never drain a cell below this, so
    // perched lakes stay filled at their basin elevation instead of flowing out.
    std::vector<f32> water_rest_level;
    // ---: fixed-point FLOWING-water state (HASHED; millimetres, deterministic) ---
    // The virtual-pipes (Mei) solver runs on integers so host==peer is bit-exact (the hash
    // FNV-1a's the raw bits). Surface height = water_bed_mm + water_depth_mm. The float arrays
    // above become  mirrors (water_level_data regenerated from mm for the mesher).
    std::vector<std::int32_t>
        water_depth_mm;                     // water depth above bed (mm, >= 0). size = resolution^2
    std::vector<std::int32_t> water_bed_mm; // terrain bed height (mm). re-sampled on edit ()
    // Per-edge persisted outflow flux (mm-vol/tick), signed: +q drains the lower-index cell toward
    // its +X or +Z neighbour. Layout: [2*i + 0] = +X edge of cell i, [2*i + 1] = +Z edge. size =
    // 2*res^2.
    std::vector<std::int32_t> water_edge_flux;
    // (water performance contract) Cached per-cell RIVER SOURCE mask (mm/tick):
    // RIVER_DISCHARGE_MM where RiverInfluenceAt(cell) >= threshold, else 0. A pure function of cell
    // position, so it is computed ONCE (lazily, size-guarded in StepChunkWaterFixed) instead of
    // re-evaluating the noise every tick. NOT serialized and NOT hashed (a derived accelerator);
    // size = resolution^2.
    std::vector<std::int32_t> water_src_mm;
    std::atomic<bool> has_water_sim{false};
    std::atomic<bool> water_mesh_generated{false};
    std::atomic<int> current_water_resolution{8}; // Current water grid resolution (4, 8, 16, or 32)

    // <<< OPTIMIZATION: Activity Culling State >>>
    std::atomic<bool> is_water_sleeping{false};
    // Max change in water level from the last sim tick. Only written by the chunk's own sim job.
    float max_water_delta_last_tick{0.0f};
    // How many consecutive ticks the water has been calm. Only accessed by the main thread.
    int ticks_below_threshold{0};
    // Coalesces water render mesh invalidation so simulation ticks do not force a remesh every
    // frame.
    int water_mesh_dirty_ticks{0};

    static ChunkID calculate_id(const IVec3& coords);
    static IVec3 decode_id(ChunkID id);
    static bool is_valid_state_transition(ChunkState from, ChunkState to);
    bool try_set_state(ChunkState expected_state, ChunkState new_state);

private:
    void advance_water_seed_edit_generation() {
        auto generation = m_water_seed_edit_generation.load(std::memory_order_acquire);
        while (
            generation != std::numeric_limits<std::uint64_t>::max() &&
            !m_water_seed_edit_generation.compare_exchange_weak(
                generation, generation + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }
    std::atomic<std::uint64_t> m_water_seed_edit_generation{0};
    // Main-thread owned handle. Workers capture a separate handle, never access
    // this member, and only publish immutable inputs through the ready flag.
    std::shared_ptr<HighWaterSeedInputs> m_water_seed_inputs;
    const IVec3 m_coords;
    const ChunkID m_id;
    ChunkState m_state;
    mutable std::mutex m_state_mutex; // mutable for use in const getter
    std::atomic<bool> m_voxel_data_dirty{false};
    std::atomic<u8> m_sdf_provenance{static_cast<u8>(ChunkSdfProvenance::GeneratedCurrentParams)};
    std::atomic<u32> m_voxel_revision{0};
    std::atomic<bool> m_water_state_dirty{false};
    std::atomic<bool> m_water_authority{false};
    std::atomic<u32> m_water_revision{0};
};

} // namespace Luminumbra
