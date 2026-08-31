#pragma once

// the STATEFUL energy-field layer — the persistent half of
// the engine's emissive scalar field. The re-derivable half (AetherFieldSystem)
// is a pure function of (seed, tick, origin); THIS layer holds gameplay-caused
// deposits (input-history hysteresis, the RimWorld-pollution class) that persist,
// decay, and diffuse deterministically.
//
// TRUTH DOMAIN (-2): integer fixed-point ONLY. Cells are uint16 raw units
// (kEnergyRawPerUnit raw = 1 gameplay unit); every kernel op is integer
// multiply/shift/add — NO float exists anywhere on this path, so the state is
// bit-deterministic across runs, builds, and machines.
//
// WORLD-ANCHORED SPARSE PAGES (-9): unlike the re-derivable field's
// region-anchored scrolling grid (scrolling is free for a pure function), the
// stateful truth is keyed by WORLD cell coordinates in sparse 16x16-cell pages
// (an ordered std::map — NEVER an unordered container on a sim path). Only the
// ACTIVE WINDOW (kEnergyWindowCells^2 cells, page-aligned, anchored by the
// caller from replicated sim input — never camera state) ticks. Pages outside
// it are FROZEN with their last-touched cadence step; when a page re-enters the
// window it receives CATCH-UP DECAY as the SEQUENTIAL per-step multiply-shift
// loop (pow-by-squaring truncates once instead of per step and is NOT bit-equal
// Codex sign-off r2). The window edge is SEALED: no outflow crosses it, and
// the truncating-outflow kernel keeps the residue in the source cell, so the
// total is conserved exactly (proving signal d).
//
// KERNEL (pinned; changing ANY constant = deliberate world_hash bump when the
// owning flag is ON):
//   deposits  — every tick, TWO-PHASE: gather (emitter_id, cell, amount) then
//               sort by (cell, channel, emitter_id) and apply with saturation
//               (/ ordering law); clipped raw is returned for the
//               conservation accounting (proving signal a2).
//   decay     — v' = (v * (2^kEnergyDecayShift - kEnergyDecayD)) >> kEnergyDecayShift
//               every kEnergyCadenceTicks ticks; floor division guarantees the
//               value reaches EXACTLY 0 (no epsilon tail).
//   diffusion — kEnergyDiffuseIterations double-buffered gather sweeps on the
//               same cadence; per-neighbour outflow (v >> kEnergyDiffuseShift)
//               truncates, residue stays in the source — number-conserving by
//               construction, and the post-sweep cell provably fits uint16
//               (keep >= 12/16 v + inflow <= 4 * (65535 >> 4) == 65535).
//
// SUB-HASH (-3): CanonicalBytes emits the "aether_state:v1:" tagged
// canonical byte string over the ENTIRE nonzero page set (paged-out cells and
// their epochs INCLUDED — two sessions with equal windows but different
// paged-out state MUST differ; proving signal e), or the EMPTY string when no
// nonzero cell exists (additive empty-neutral). The caller (GameSession) wraps
// it in StableChecksum, mirroring the scent/plant sub-hash seam.
//
// PERSISTENCE (-4): SerializeRecord NORMALIZES first (catch-up decay
// brings every page current) then writes a version-tagged record carrying the
// channel count and the cadence PHASE; DeserializeRecord rebases last-touched
// state onto the loaded tick base and restores the phase, so a save/load
// mid-cycle resumes bit-exactly (proving signal c). Absent record == all zeros.
//
// Seed-offset registry (append-only): +38 energy-field-state (RE-CHARTERED from
// the stale +35, which PhotoFilters owns; +28 light-tools, +36 germination also
// taken — grep-verified 2026-07-05). The layer has NO RNG; the offset is
// recorded for collision-avoidance only and intentionally never consumed
// (the PhotoScoring +24 / PhotoFilters +35 precedent).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace luminumbra::fields {

inline constexpr const char* kEnergyFieldStateSchema = "luminumbra.fields.energy_field_state.v1";

// Reserved seed-stream offset (registry:... germination+36, season+37,
// energy-field-state+38). Recorded for collision-avoidance; never consumed.
inline constexpr std::uint64_t kEnergyFieldSeedOffset = 38ull;

// PINNED geometry. Cell size mirrors the re-derivable field so both halves
// sample the same 24 m grid identity; the active window is page-aligned.
inline constexpr int kEnergyPageCells = 16;   // cells per page side
inline constexpr int kEnergyWindowCells = 64; // active window side (4x4 pages)
inline constexpr int kEnergyRawPerUnit = 256; // raw units per gameplay unit

// PINNED kernel constants (frozen; deliberate-bump surface when ON).
inline constexpr std::uint64_t kEnergyCadenceTicks = 8ull;
inline constexpr int kEnergyDiffuseIterations = 4;
inline constexpr unsigned kEnergyDiffuseShift = 4; // outflow = v >> 4 per side
inline constexpr unsigned kEnergyDecayShift = 10;  // v' = v*(1024-d) >> 10
inline constexpr unsigned kEnergyDecayD = 8;       // ~0.78% per cadence step

class EnergyFieldState {
public:
    // channels >= 1. v1 ships single-channel;  constructs with 2
    // (Lumin/Umbra polarity) — the record header carries the count either way.
    explicit EnergyFieldState(int channels = 1);

    [[nodiscard]] int channels() const noexcept {
        return m_channels;
    }

    // Anchor the active window. cx/cz are WORLD CELL coordinates (the caller
    // quantizes a world position by the shared 24 m cell size, from replicated
    // sim input only). The window origin snaps to page boundaries so pages are
    // never split across the seam.
    void SetAnchorCell(int cx, int cz);

    //  of the deposit path: gather. Applied (sorted, saturating) by the
    // next Tick. Deposits landing OUTSIDE the active window are DROPPED and
    // counted as clipped (only the window ticks; emitters are participant-gated
    // to it — the Factorio inactive-chunk rule).
    void
    QueueDeposit(std::uint64_t emitter_id, int cx, int cz, int channel, std::uint32_t amount_raw);

    // Advance to `tick`: catch-up any page re-entering the window, apply the
    // sorted deposit buffer, then (on the cadence, honouring the persisted
    // phase) decay + diffuse the window with sealed edges. Returns the raw
    // amount CLIPPED this tick (saturation + out-of-window drops) for the
    // conservation accounting (proving signal a2).
    std::uint64_t Tick(std::uint64_t tick);

    // Point read at world cell coords (any page, frozen or active; 0 when the
    // cell/page does not exist). Frozen pages are read AS STORED — catch-up
    // happens on window re-entry, not on read (reads must not mutate).
    [[nodiscard]] std::uint16_t at_cell(int cx, int cz, int channel = 0) const;

    // Sum of every cell across every page (test/accounting aid).
    [[nodiscard]] std::uint64_t total_raw() const;

    // The "aether_state:v1:" canonical byte string over the whole nonzero page
    // set, or "" when all-zero. The caller applies StableChecksum.
    [[nodiscard]] std::string CanonicalBytes() const;

    // Persistence (-4). Serialize normalizes (catch-up to save_tick's
    // cadence step) then emits the record; Deserialize replaces this object's
    // state, rebasing onto load_tick_base and restoring the cadence phase.
    [[nodiscard]] std::string SerializeRecord(std::uint64_t save_tick);
    bool DeserializeRecord(const std::string& record, std::uint64_t load_tick_base);

    // Diagnostics.
    [[nodiscard]] std::size_t page_count() const noexcept {
        return m_pages.size();
    }
    [[nodiscard]] std::uint64_t fires_completed() const noexcept {
        return m_fires_completed;
    }
    [[nodiscard]] std::uint64_t next_fire_tick() const noexcept {
        return m_next_fire_tick;
    }

private:
    struct Page {
        // cell-major, channel-interleaved: values[(lz*kEnergyPageCells+lx)*channels+ch]
        std::vector<std::uint16_t> values;
        std::uint64_t last_step = 0; // cadence step this page was last current at
        [[nodiscard]] bool all_zero() const noexcept;
    };

    struct PendingDeposit {
        std::int64_t cell_key = 0;
        int channel = 0;
        std::uint64_t emitter_id = 0;
        std::uint32_t amount = 0;
    };

    [[nodiscard]] static std::int64_t PackPageKey(int px, int pz) noexcept;
    [[nodiscard]] static std::int64_t PackCellKey(int cx, int cz) noexcept;
    static void UnpackCellKey(std::int64_t key, int& cx, int& cz) noexcept;

    [[nodiscard]] bool InWindow(int cx, int cz) const noexcept;

    // Sequential per-step decay of one value over `steps` cadence steps
    // (bit-equal to the in-window path; early-exits at 0).
    [[nodiscard]] static std::uint16_t DecaySteps(std::uint16_t v, std::uint64_t steps) noexcept;

    Page& EnsurePage(int px, int pz);
    void CatchUpWindowPages();
    std::uint64_t ApplyDeposits();
    void DecayAndDiffuseWindow();
    void StampWindowPages();
    void DropZeroPages();
    void NormalizeAllPages();

    int m_channels = 1;
    // Window origin in world cell coords, page-aligned. Valid once anchored.
    int m_window_ox = 0;
    int m_window_oz = 0;
    bool m_anchored = false;
    // Cadence bookkeeping. `m_fires_completed` is a RELATIVE counter (pages
    // stamp against it; ages in the sub-hash/record are differences, never
    // absolute ticks), and `m_next_fire_tick` is the absolute tick of the next
    // decay+diffusion firing — the quantity the save record's "remaining"
    // rebases across the load-resets-tick-to-zero boundary (-4).
    std::uint64_t m_fires_completed = 0;
    std::uint64_t m_next_fire_tick = kEnergyCadenceTicks;
    std::map<std::int64_t, Page> m_pages; // ORDERED — deterministic iteration
    std::vector<PendingDeposit> m_pending;
};

} // namespace luminumbra::fields
