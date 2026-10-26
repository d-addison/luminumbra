#include "EnergyFieldState.h"

#include "../world/ActiveRegionLedger.h"
#include "../world/WorldClock.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace luminumbra::fields {

namespace {

// Floor division for window + page math on negative world cells.
constexpr int FloorDiv(int a, int b) noexcept {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

constexpr std::uint16_t
SaturatingAdd16(std::uint16_t v, std::uint32_t add, std::uint32_t& clipped) noexcept {
    const std::uint32_t sum = static_cast<std::uint32_t>(v) + add;
    if (sum > 0xFFFFu) {
        clipped += sum - 0xFFFFu;
        return 0xFFFFu;
    }
    return static_cast<std::uint16_t>(sum);
}

// One pinned decay step: v' = (v * (2^shift - d)) >> shift. Floor division
// guarantees strict decrease of >= 1 once v < 2^shift / d, so the value
// reaches EXACTLY 0 ( -2).
constexpr std::uint16_t DecayOnce(std::uint16_t v) noexcept {
    constexpr std::uint32_t kMul = (1u << kEnergyDecayShift) - kEnergyDecayD;
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(v) * kMul) >> kEnergyDecayShift);
}

} // namespace

bool EnergyFieldState::Page::all_zero() const noexcept {
    if (regional_summary)
        return !regional_summary->any;
    for (const std::uint16_t v : values) {
        if (v != 0)
            return false;
    }
    return true;
}

EnergyFieldState::EnergyFieldState(int channels)
    : m_channels(channels < 1 ? 1 : channels) {}

std::int64_t EnergyFieldState::PackPageKey(int px, int pz) noexcept {
    return (static_cast<std::int64_t>(px) * 4294967296ll) |
           (static_cast<std::int64_t>(static_cast<std::uint32_t>(pz)));
}

std::int64_t EnergyFieldState::PackCellKey(int cx, int cz) noexcept {
    return (static_cast<std::int64_t>(cx) * 4294967296ll) |
           (static_cast<std::int64_t>(static_cast<std::uint32_t>(cz)));
}

void EnergyFieldState::UnpackCellKey(std::int64_t key, int& cx, int& cz) noexcept {
    cx = static_cast<int>(key >> 32);
    cz = static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFFll));
}

void EnergyFieldState::SetAnchorCell(int cx, int cz) {
    // Center the window on the anchor, snapped to page boundaries so a page is
    // never split across the window seam (-9).
    const int half = kEnergyWindowCells / 2;
    m_window_ox = FloorDiv(cx - half, kEnergyPageCells) * kEnergyPageCells;
    m_window_oz = FloorDiv(cz - half, kEnergyPageCells) * kEnergyPageCells;
    m_anchored = true;
}

bool EnergyFieldState::InWindow(int cx, int cz) const noexcept {
    return m_anchored && cx >= m_window_ox && cx < m_window_ox + kEnergyWindowCells &&
           cz >= m_window_oz && cz < m_window_oz + kEnergyWindowCells;
}

void EnergyFieldState::QueueDeposit(
    std::uint64_t emitter_id, int cx, int cz, int channel, std::uint32_t amount_raw) {
    if (channel < 0 || channel >= m_channels || amount_raw == 0)
        return;
    m_pending.push_back(PendingDeposit{PackCellKey(cx, cz), channel, emitter_id, amount_raw});
}

std::uint16_t EnergyFieldState::DecaySteps(std::uint16_t v, std::uint64_t steps) noexcept {
    // SEQUENTIAL per-step loop — bit-equal to the in-window path (a
    // pow-by-squaring shortcut truncates once instead of per step and is NOT
    // bit-equal). Bounded: the exact-zero property caps it at a few hundred
    // iterations for any uint16 value; the early-exit fires long before that
    // for old pages.
    for (std::uint64_t s = 0; s < steps && v != 0; ++s) {
        v = DecayOnce(v);
    }
    return v;
}

EnergyFieldState::Page& EnergyFieldState::EnsurePage(int px, int pz) {
    const std::int64_t key = PackPageKey(px, pz);
    auto it = m_pages.find(key);
    if (it == m_pages.end()) {
        Page page;
        page.values.assign(static_cast<std::size_t>(kEnergyPageCells) * kEnergyPageCells *
                               static_cast<std::size_t>(m_channels),
                           0);
        page.last_step = m_fires_completed;
        it = m_pages.emplace(key, std::move(page)).first;
    }
    it->second.regional_summary.reset();
    return it->second;
}

void EnergyFieldState::CatchUpWindowPages() {
    // Bring window pages current through every COMPLETED firing they missed
    // while frozen outside the window. A page stamped at firing N and caught
    // up at m_fires_completed == M receives exactly M - N decay steps — the
    // same count an in-window page received live (proving signal:
    // SequentialCatchUpBitEqual).
    if (!m_anchored)
        return;
    const int pages_per_side = kEnergyWindowCells / kEnergyPageCells;
    const int px0 = FloorDiv(m_window_ox, kEnergyPageCells);
    const int pz0 = FloorDiv(m_window_oz, kEnergyPageCells);
    for (int pz = pz0; pz < pz0 + pages_per_side; ++pz) {
        for (int px = px0; px < px0 + pages_per_side; ++px) {
            auto it = m_pages.find(PackPageKey(px, pz));
            if (it == m_pages.end())
                continue;
            Page& page = it->second;
            if (page.last_step >= m_fires_completed)
                continue;
            const std::uint64_t elapsed = m_fires_completed - page.last_step;
            page.regional_summary.reset();
            for (std::uint16_t& v : page.values) {
                v = DecaySteps(v, elapsed);
            }
            page.last_step = m_fires_completed;
        }
    }
}

std::uint64_t EnergyFieldState::ApplyDeposits() {
    if (m_pending.empty())
        return 0;
    //  sort by (cell, channel, emitter_id, amount) — the /
    //  ordering law: registration/iteration order can never reach the
    // field bytes.
    std::sort(
        m_pending.begin(), m_pending.end(), [](const PendingDeposit& a, const PendingDeposit& b) {
            if (a.cell_key != b.cell_key)
                return a.cell_key < b.cell_key;
            if (a.channel != b.channel)
                return a.channel < b.channel;
            if (a.emitter_id != b.emitter_id)
                return a.emitter_id < b.emitter_id;
            return a.amount < b.amount;
        });

    std::uint64_t clipped_total = 0;
    for (const PendingDeposit& d : m_pending) {
        int cx = 0, cz = 0;
        UnpackCellKey(d.cell_key, cx, cz);
        if (!InWindow(cx, cz)) {
            // Only the active window ticks; out-of-window deposits are dropped
            // and accounted (the Factorio inactive-chunk rule, header note).
            clipped_total += d.amount;
            continue;
        }
        const int px = FloorDiv(cx, kEnergyPageCells);
        const int pz = FloorDiv(cz, kEnergyPageCells);
        Page& page = EnsurePage(px, pz);
        const int lx = cx - px * kEnergyPageCells;
        const int lz = cz - pz * kEnergyPageCells;
        const std::size_t idx =
            (static_cast<std::size_t>(lz) * kEnergyPageCells + static_cast<std::size_t>(lx)) *
                static_cast<std::size_t>(m_channels) +
            static_cast<std::size_t>(d.channel);
        std::uint32_t clipped = 0;
        page.values[idx] = SaturatingAdd16(page.values[idx], d.amount, clipped);
        clipped_total += clipped;
    }
    m_pending.clear();
    return clipped_total;
}

void EnergyFieldState::DecayAndDiffuseWindow() {
    if (!m_anchored)
        return;
    const int n = kEnergyWindowCells;
    const int pages_per_side = n / kEnergyPageCells;
    const int px0 = FloorDiv(m_window_ox, kEnergyPageCells);
    const int pz0 = FloorDiv(m_window_oz, kEnergyPageCells);

    std::vector<std::uint16_t> win(static_cast<std::size_t>(n) * n, 0);
    std::vector<std::uint32_t> next(static_cast<std::size_t>(n) * n, 0);

    for (int ch = 0; ch < m_channels; ++ch) {
        // Load the dense window for this channel (missing page == zeros).
        bool any = false;
        for (int pz = 0; pz < pages_per_side; ++pz) {
            for (int px = 0; px < pages_per_side; ++px) {
                const auto it = m_pages.find(PackPageKey(px0 + px, pz0 + pz));
                if (it == m_pages.end())
                    continue;
                const Page& page = it->second;
                for (int lz = 0; lz < kEnergyPageCells; ++lz) {
                    for (int lx = 0; lx < kEnergyPageCells; ++lx) {
                        const std::uint16_t v =
                            page.values[(static_cast<std::size_t>(lz) * kEnergyPageCells +
                                         static_cast<std::size_t>(lx)) *
                                            static_cast<std::size_t>(m_channels) +
                                        static_cast<std::size_t>(ch)];
                        if (v == 0)
                            continue;
                        win[static_cast<std::size_t>(pz * kEnergyPageCells + lz) * n +
                            static_cast<std::size_t>(px * kEnergyPageCells + lx)] = v;
                        any = true;
                    }
                }
            }
        }
        if (any) {
            // Decay (one cadence step), then the pinned diffusion sweeps.
            // SEALED window edge: an out-of-window neighbour contributes and
            // receives nothing; the truncating outflow keeps the residue in
            // the source, so the window total is conserved exactly (proving
            // signal d: WindowEdgeConservation).
            for (std::uint16_t& v : win)
                v = DecayOnce(v);

            for (int sweep = 0; sweep < kEnergyDiffuseIterations; ++sweep) {
                std::fill(next.begin(), next.end(), 0u);
                for (int z = 0; z < n; ++z) {
                    for (int x = 0; x < n; ++x) {
                        const std::size_t i =
                            static_cast<std::size_t>(z) * n + static_cast<std::size_t>(x);
                        const std::uint32_t v = win[i];
                        if (v == 0)
                            continue;
                        const std::uint32_t out = v >> kEnergyDiffuseShift;
                        std::uint32_t sent = 0;
                        if (out > 0) {
                            if (x > 0) {
                                next[i - 1] += out;
                                sent += out;
                            }
                            if (x + 1 < n) {
                                next[i + 1] += out;
                                sent += out;
                            }
                            if (z > 0) {
                                next[i - static_cast<std::size_t>(n)] += out;
                                sent += out;
                            }
                            if (z + 1 < n) {
                                next[i + static_cast<std::size_t>(n)] += out;
                                sent += out;
                            }
                        }
                        next[i] += v - sent; // residue stays in the source
                    }
                }
                // Post-sweep values provably fit uint16 (header proof:
                // keep >= 12/16 v, inflow <= 4 * (65535 >> 4)), so this
                // narrowing is exact — never a hidden saturation.
                for (std::size_t i = 0; i < win.size(); ++i) {
                    win[i] = static_cast<std::uint16_t>(next[i]);
                }
            }

            // Write back, creating pages only where nonzero values need
            // storage (diffusion spreading into a page-less area).
            for (int pz = 0; pz < pages_per_side; ++pz) {
                for (int px = 0; px < pages_per_side; ++px) {
                    const std::int64_t key = PackPageKey(px0 + px, pz0 + pz);
                    auto it = m_pages.find(key);
                    bool page_needed = (it != m_pages.end());
                    if (!page_needed) {
                        for (int lz = 0; lz < kEnergyPageCells && !page_needed; ++lz) {
                            for (int lx = 0; lx < kEnergyPageCells; ++lx) {
                                if (win[static_cast<std::size_t>(pz * kEnergyPageCells + lz) * n +
                                        static_cast<std::size_t>(px * kEnergyPageCells + lx)] !=
                                    0) {
                                    page_needed = true;
                                    break;
                                }
                            }
                        }
                        if (!page_needed)
                            continue;
                    }
                    Page& page =
                        (it != m_pages.end()) ? it->second : EnsurePage(px0 + px, pz0 + pz);
                    page.regional_summary.reset();
                    for (int lz = 0; lz < kEnergyPageCells; ++lz) {
                        for (int lx = 0; lx < kEnergyPageCells; ++lx) {
                            page.values[(static_cast<std::size_t>(lz) * kEnergyPageCells +
                                         static_cast<std::size_t>(lx)) *
                                            static_cast<std::size_t>(m_channels) +
                                        static_cast<std::size_t>(ch)] =
                                win[static_cast<std::size_t>(pz * kEnergyPageCells + lz) * n +
                                    static_cast<std::size_t>(px * kEnergyPageCells + lx)];
                        }
                    }
                }
            }
            std::fill(win.begin(), win.end(), 0);
        }
    }
}

void EnergyFieldState::StampWindowPages() {
    const int pages_per_side = kEnergyWindowCells / kEnergyPageCells;
    const int px0 = FloorDiv(m_window_ox, kEnergyPageCells);
    const int pz0 = FloorDiv(m_window_oz, kEnergyPageCells);
    for (int pz = pz0; pz < pz0 + pages_per_side; ++pz) {
        for (int px = px0; px < px0 + pages_per_side; ++px) {
            auto it = m_pages.find(PackPageKey(px, pz));
            if (it != m_pages.end())
                it->second.last_step = m_fires_completed;
        }
    }
}

void EnergyFieldState::DropZeroPages() {
    for (auto it = m_pages.begin(); it != m_pages.end();) {
        if (it->second.all_zero()) {
            it = m_pages.erase(it);
        } else {
            ++it;
        }
    }
}

std::uint64_t EnergyFieldState::Tick(std::uint64_t tick) {
    // Pages re-entering the window catch up BEFORE deposits land on them.
    CatchUpWindowPages();

    const std::uint64_t clipped = ApplyDeposits();

    // Fire the cadence (a while-loop so a caller skipping ticks stays
    // deterministic: every due firing runs, in order).
    while (m_anchored && tick >= m_next_fire_tick) {
        DecayAndDiffuseWindow();
        ++m_fires_completed;
        m_next_fire_tick += kEnergyCadenceTicks;
        StampWindowPages();
        DropZeroPages();
    }
    return clipped;
}

std::uint16_t EnergyFieldState::at_cell(int cx, int cz, int channel) const {
    if (channel < 0 || channel >= m_channels)
        return 0;
    const int px = FloorDiv(cx, kEnergyPageCells);
    const int pz = FloorDiv(cz, kEnergyPageCells);
    const auto it = m_pages.find(PackPageKey(px, pz));
    if (it == m_pages.end())
        return 0;
    const int lx = cx - px * kEnergyPageCells;
    const int lz = cz - pz * kEnergyPageCells;
    return it->second
        .values[(static_cast<std::size_t>(lz) * kEnergyPageCells + static_cast<std::size_t>(lx)) *
                    static_cast<std::size_t>(m_channels) +
                static_cast<std::size_t>(channel)];
}

std::uint64_t EnergyFieldState::total_raw() const {
    std::uint64_t total = 0;
    for (const auto& [key, page] : m_pages) {
        for (const std::uint16_t v : page.values)
            total += v;
    }
    return total;
}

std::string EnergyFieldState::CanonicalBytes() const {
    // The ENTIRE nonzero page set, paged-out cells and their AGES included
    // (-3 / proving signal e: two sessions with equal active windows but
    // different paged-out state MUST differ). Ages are relative
    // (fires_completed - last_step), so the bytes are reproducible across runs
    // regardless of absolute session ticks. Empty string when no nonzero cell
    // exists (additive empty-neutral — the scent/plant contract).
    std::ostringstream bytes;
    bool any = false;
    for (const auto& [key, page] : m_pages) {
        bool page_any = false;
        for (const std::uint16_t v : page.values) {
            if (v != 0) {
                page_any = true;
                break;
            }
        }
        if (!page_any)
            continue;
        if (!any) {
            bytes << "aether_state:v1:" << m_channels << ':';
            any = true;
        }
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        const std::uint64_t age =
            m_fires_completed >= page.last_step ? m_fires_completed - page.last_step : 0;
        bytes << 'P' << px << ',' << pz << ',' << age << ':';
        for (std::size_t i = 0; i < page.values.size(); ++i) {
            if (page.values[i] != 0) {
                bytes << i << '=' << page.values[i] << ';';
            }
        }
    }
    if (!any)
        return {};
    return bytes.str();
}

void EnergyFieldState::NormalizeAllPages() {
    for (auto& [key, page] : m_pages) {
        if (page.last_step >= m_fires_completed)
            continue;
        const std::uint64_t elapsed = m_fires_completed - page.last_step;
        page.regional_summary.reset();
        for (std::uint16_t& v : page.values) {
            v = DecaySteps(v, elapsed);
        }
        page.last_step = m_fires_completed;
    }
    DropZeroPages();
}

std::string EnergyFieldState::SerializeRecord(std::uint64_t save_tick) {
    // Normalize: bring EVERY page current through the completed firings, so
    // the record is a pure snapshot and last-step bookkeeping restarts at zero
    // on load (frozen-page ages collapse into the values themselves).
    NormalizeAllPages();

    // Ticks remaining until the next cadence firing — the epoch-rebase
    // quantity (-4): load restores the phase from it so the next
    // diffusion pass fires at the same relative step even though load resets
    // the sim tick stream.
    const std::uint64_t remaining =
        m_next_fire_tick > save_tick ? m_next_fire_tick - save_tick : kEnergyCadenceTicks;

    std::ostringstream out;
    out << "EFS1 " << m_channels << ' ' << remaining << '\n';
    for (const auto& [key, page] : m_pages) {
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        out << 'P' << ' ' << px << ' ' << pz;
        for (std::size_t i = 0; i < page.values.size(); ++i) {
            if (page.values[i] != 0) {
                out << ' ' << i << ':' << page.values[i];
            }
        }
        out << '\n';
    }
    return out.str();
}

void EnergyFieldState::ResetEmptyAtTick(std::uint64_t tick) {
    m_pages.clear();
    m_pending.clear();
    m_fires_completed = 0;
    m_next_fire_tick = tick + (kEnergyCadenceTicks - tick % kEnergyCadenceTicks);
}

bool EnergyFieldState::DeserializeRecord(const std::string& record, std::uint64_t load_tick_base) {
    std::istringstream in(record);
    std::string magic;
    int channels = 0;
    std::uint64_t remaining = 0;
    if (!(in >> magic >> channels >> remaining) || magic != "EFS1" || channels < 1 ||
        remaining < 1 || remaining > kEnergyCadenceTicks) {
        return false;
    }
    std::map<std::int64_t, Page> pages;
    std::string line;
    std::getline(in, line); // consume the header line's remainder
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::istringstream ls(line);
        char tag = 0;
        int px = 0, pz = 0;
        if (!(ls >> tag >> px >> pz) || tag != 'P')
            return false;
        Page page;
        page.values.assign(static_cast<std::size_t>(kEnergyPageCells) * kEnergyPageCells *
                               static_cast<std::size_t>(channels),
                           0);
        page.last_step = 0;
        std::string cell;
        while (ls >> cell) {
            const std::size_t colon = cell.find(':');
            if (colon == std::string::npos)
                return false;
            std::size_t idx = 0;
            unsigned long val = 0;
            try {
                idx = static_cast<std::size_t>(std::stoull(cell.substr(0, colon)));
                val = std::stoul(cell.substr(colon + 1));
            } catch (...) {
                return false;
            }
            if (idx >= page.values.size() || val > 0xFFFFu || val == 0)
                return false;
            page.values[idx] = static_cast<std::uint16_t>(val);
        }
        pages.emplace(PackPageKey(px, pz), std::move(page));
    }

    m_channels = channels;
    m_pages = std::move(pages);
    m_pending.clear();
    // Epoch rebase: the record is normalized (all pages current), so the
    // relative counter restarts at zero and the next firing lands `remaining`
    // ticks after the load base — the saved cadence phase carried across the
    // tick-reset boundary.
    m_fires_completed = 0;
    m_next_fire_tick = load_tick_base + remaining;
    return true;
}

namespace {
std::int64_t RegionalFloorDiv(std::int64_t value, std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return quotient - (value % divisor < 0 ? 1 : 0);
}
bool ValidEnergyOwner(EnergyFieldState::RegionKey key) noexcept {
    return key.x >= -32768 && key.x < 32768 && key.z >= -32768 && key.z < 32768;
}
std::pair<int, int> RegionalCellRange(std::int32_t region) noexcept {
    const auto start = std::int64_t{region} * 512 - 12;
    return {static_cast<int>(-RegionalFloorDiv(-start, 24)),
            static_cast<int>(-RegionalFloorDiv(-(start + 512), 24))};
}
template<typename Integer>
bool ParseRegionalInteger(const std::string& token, Integer& value) {
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}
} // namespace

EnergyFieldState::RegionKey EnergyFieldState::OwnerOfCell(int cx, int cz) noexcept {
    // Integer centre mapping also handles negative cells without float rounding.
    return {static_cast<std::int32_t>(RegionalFloorDiv(std::int64_t{cx} * 24 + 12, 512)),
            static_cast<std::int32_t>(RegionalFloorDiv(std::int64_t{cz} * 24 + 12, 512))};
}

bool EnergyFieldState::QueueRegionalDeposit(
    std::uint64_t emitter_id, int cx, int cz, int channel, std::uint32_t amount_raw) {
    if (m_channels < 1 || m_channels > 64 || channel < 0 || channel >= m_channels ||
        !ValidEnergyOwner(OwnerOfCell(cx, cz)) || m_pending.size() >= kMaxRegionalDeposits)
        return false;
    if (amount_raw != 0)
        m_pending.push_back({PackCellKey(cx, cz), channel, emitter_id, amount_raw});
    return true;
}

bool EnergyFieldState::OwnsRegion(RegionKey owner) const {
    for (const auto& deposit : m_pending) {
        int cx = 0, cz = 0;
        UnpackCellKey(deposit.cell_key, cx, cz);
        if (OwnerOfCell(cx, cz) == owner)
            return true;
    }
    const auto cell_range = [](std::int32_t axis) {
        const auto start = std::int64_t{axis} * 512 - 12;
        return std::pair{
            std::max(-RegionalFloorDiv(-start, 24), std::int64_t{std::numeric_limits<int>::min()}),
            std::min(-RegionalFloorDiv(-(start + 512), 24),
                     std::int64_t{std::numeric_limits<int>::max()} + 1)};
    };
    const auto [x0, x1] = cell_range(owner.x);
    const auto [z0, z1] = cell_range(owner.z);
    if (x0 >= x1 || z0 >= z1)
        return false;
    // At most 3 x 3 overlapping pages and 22 x 22 cells (each with m_channels
    // values). No serialization, allocation, age normalization or unrelated page scan.
    for (auto pz = RegionalFloorDiv(z0, kEnergyPageCells);
         pz <= RegionalFloorDiv(z1 - 1, kEnergyPageCells);
         ++pz) {
        for (auto px = RegionalFloorDiv(x0, kEnergyPageCells);
             px <= RegionalFloorDiv(x1 - 1, kEnergyPageCells);
             ++px) {
            const auto page = m_pages.find(PackPageKey(static_cast<int>(px), static_cast<int>(pz)));
            if (page == m_pages.end())
                continue;
            const auto min_x = std::max(x0, px * kEnergyPageCells) - px * kEnergyPageCells;
            const auto end_x = std::min(x1, (px + 1) * kEnergyPageCells) - px * kEnergyPageCells;
            for (auto row = std::max(z0, pz * kEnergyPageCells);
                 row < std::min(z1, (pz + 1) * kEnergyPageCells);
                 ++row) {
                const auto offset = (row - pz * kEnergyPageCells) * kEnergyPageCells;
                // Keep legacy channel arithmetic wide even on 32-bit hosts, and intersect
                // with the actual stored values before narrowing an index.
                const auto end = std::min(static_cast<std::uint64_t>(offset + end_x) *
                                              static_cast<std::uint64_t>(m_channels),
                                          static_cast<std::uint64_t>(page->second.values.size()));
                for (auto i = static_cast<std::uint64_t>(offset + min_x) *
                              static_cast<std::uint64_t>(m_channels);
                     i < end;
                     ++i)
                    if (page->second.values[static_cast<std::size_t>(i)] != 0)
                        return true;
            }
        }
    }
    return false;
}

const EnergyFieldState::RegionalPageFacts&
EnergyFieldState::Page::regional_facts(std::int64_t key, int channels) const {
    if (regional_summary)
        return *regional_summary;
    RegionalPageFacts facts;
    int px = 0, pz = 0;
    UnpackCellKey(key, px, pz);
    facts.valid = channels >= 1 && channels <= 64 &&
                  values.size() == std::size_t{kEnergyPageCells} * kEnergyPageCells *
                                       static_cast<std::size_t>(channels) &&
                  px >= -43691 && px <= 43690 && pz >= -43691 && pz <= 43690;
    const auto width = [](std::size_t value) {
        std::size_t digits = 1;
        while (value >= 10) {
            value /= 10;
            ++digits;
        }
        return digits;
    };
    const auto header_bytes = 4 + std::to_string(px).size() + std::to_string(pz).size();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == 0)
            continue;
        if (!facts.any)
            facts.record_bytes = header_bytes;
        facts.any = true;
        // Saturate the diagnostic at refusal, so even an incompatible legacy page
        // cannot overflow the length calculation. No emitted record changes.
        const auto token_bytes = 2 + width(i) + width(values[i]);
        if (facts.record_bytes > kMaxRegionalRecordBytes ||
            token_bytes > kMaxRegionalRecordBytes - facts.record_bytes)
            facts.record_bytes = kMaxRegionalRecordBytes + 1;
        else
            facts.record_bytes += token_bytes;
        if (!facts.valid)
            continue;
        const auto cell = i / static_cast<std::size_t>(channels);
        const auto owner =
            OwnerOfCell(px * kEnergyPageCells + static_cast<int>(cell % kEnergyPageCells),
                        pz * kEnergyPageCells + static_cast<int>(cell / kEnergyPageCells));
        if (!ValidEnergyOwner(owner)) {
            facts.valid = false;
            continue;
        }
        bool listed = false;
        for (std::size_t j = 0; j < facts.owner_count; ++j)
            listed = listed || facts.owners[j] == owner;
        if (!listed) {
            if (facts.owner_count == facts.owners.size())
                facts.valid = false;
            else
                facts.owners[facts.owner_count++] = owner;
        }
    }
    regional_summary = facts;
    return *regional_summary;
}

std::string EnergyFieldState::RegionBytes(RegionKey owner) const {
    if (!ValidEnergyOwner(owner))
        return {};
    // One 512 m owner intersects at most 3 x 3 physical pages. Preserve the
    // canonical signed packed-key order, including across negative z coordinates.
    std::array<std::int64_t, 9> keys{};
    std::size_t key_count = 0;
    const auto [min_x, end_x] = RegionalCellRange(owner.x);
    const auto [min_z, end_z] = RegionalCellRange(owner.z);
    for (int px = FloorDiv(min_x, kEnergyPageCells); px <= FloorDiv(end_x - 1, kEnergyPageCells);
         ++px)
        for (int pz = FloorDiv(min_z, kEnergyPageCells);
             pz <= FloorDiv(end_z - 1, kEnergyPageCells);
             ++pz)
            keys[key_count++] = PackPageKey(px, pz);
    // Insertion sort keeps every access explicitly within the nine-key buffer,
    // without a variable iterator range through the library's heap-sort path.
    const auto sorted_count = std::min(key_count, keys.size());
    for (std::size_t i = 1; i < sorted_count; ++i) {
        const auto key = keys[i];
        auto position = i;
        while (position > 0 && key < keys[position - 1]) {
            keys[position] = keys[position - 1];
            --position;
        }
        keys[position] = key;
    }
    std::ostringstream bytes;
    bytes.imbue(std::locale::classic());
    bool any = false;
    for (std::size_t page_index = 0; page_index < sorted_count; ++page_index) {
        const auto key = keys[page_index];
        const auto found = m_pages.find(key);
        if (found == m_pages.end())
            continue;
        const auto& page = found->second;
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        bool page_any = false;
        for (std::size_t i = 0; i < page.values.size(); ++i) {
            if (page.values[i] == 0)
                continue;
            const auto cell = i / static_cast<std::size_t>(m_channels);
            const auto cx = std::int64_t{px} * kEnergyPageCells +
                            static_cast<std::int64_t>(cell % kEnergyPageCells);
            const auto cz = std::int64_t{pz} * kEnergyPageCells +
                            static_cast<std::int64_t>(cell / kEnergyPageCells);
            if (cx < std::numeric_limits<int>::min() || cx > std::numeric_limits<int>::max() ||
                cz < std::numeric_limits<int>::min() || cz > std::numeric_limits<int>::max() ||
                OwnerOfCell(static_cast<int>(cx), static_cast<int>(cz)) != owner)
                continue;
            if (!any) {
                bytes << "aether_state:v1:" << m_channels << ':';
                any = true;
            }
            if (!page_any) {
                bytes << 'P' << px << ',' << pz << ",0:";
                page_any = true;
            }
            bytes << i << '=' << page.values[i] << ';';
        }
    }
    return bytes.str();
}

std::string EnergyFieldState::CanonicalRegionalBytes() const {
    std::ostringstream bytes;
    bytes.imbue(std::locale::classic());
    bool any = false;
    for (const auto& [key, page] : m_pages) {
        if (page.all_zero())
            continue;
        if (!any) {
            bytes << "aether_state:v1:" << m_channels << ':';
            any = true;
        }
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        bytes << 'P' << px << ',' << pz << ",0:";
        for (std::size_t i = 0; i < page.values.size(); ++i)
            if (page.values[i] != 0)
                bytes << i << '=' << page.values[i] << ';';
    }
    return bytes.str();
}

std::string EnergyFieldState::SerializeRegionalRecord() const {
    if (!RegionalRecordFits())
        throw std::length_error("Regional energy record exceeds its capacity.");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    bool any = false;
    for (const auto& [key, page] : m_pages) {
        if (page.all_zero())
            continue;
        if (!any) {
            out << "EFS1 " << m_channels << " 0\n";
            any = true;
        }
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        out << "P " << px << ' ' << pz;
        for (std::size_t i = 0; i < page.values.size(); ++i)
            if (page.values[i] != 0)
                out << ' ' << i << ':' << page.values[i];
        out << '\n';
    }
    return out.str();
}

bool EnergyFieldState::RegionalRecordFits(const EnergyFieldState* replacements) const {
    if (m_channels < 1 || m_channels > 64)
        return false;
    auto stored_pages = m_pages.size();
    if (replacements)
        for (const auto& [key, page] : replacements->m_pages) {
            (void)page;
            if (!m_pages.contains(key))
                ++stored_pages;
        }
    if (stored_pages > kMaxRegionalValueBytes / (std::size_t{kEnergyPageCells} * kEnergyPageCells *
                                                 m_channels * sizeof(std::uint16_t)))
        return false;
    const auto width = [](std::size_t value) {
        std::size_t digits = 1;
        while (value >= 10) {
            value /= 10;
            ++digits;
        }
        return digits;
    };
    std::size_t bytes = 8 + width(static_cast<std::size_t>(m_channels));
    const auto measure = [&](std::int64_t key, const Page& page) {
        const auto count = page.regional_facts(key, m_channels).record_bytes;
        if (count > kMaxRegionalRecordBytes - bytes)
            return false;
        bytes += count;
        return true;
    };
    for (const auto& [key, page] : m_pages)
        if ((!replacements || !replacements->m_pages.contains(key)) && !measure(key, page))
            return false;
    if (replacements)
        for (const auto& [key, page] : replacements->m_pages)
            if (!measure(key, page))
                return false;
    return bytes <= kMaxRegionalRecordBytes;
}

bool EnergyFieldState::DeserializeRegionalRecord(const std::string& record) {
    if (record.empty() || record.size() > kMaxRegionalRecordBytes)
        return false;
    std::istringstream in(record);
    in.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(in, line))
        return false;
    std::istringstream header(line);
    header.imbue(std::locale::classic());
    std::string magic, count_token, phase_token, extra;
    int channels = 0;
    std::uint64_t phase = 0;
    if (!(header >> magic >> count_token >> phase_token) || (header >> extra) || magic != "EFS1" ||
        !ParseRegionalInteger(count_token, channels) || channels < 1 || channels > 64 ||
        !ParseRegionalInteger(phase_token, phase) || phase != 0)
        return false;
    EnergyFieldState staged(channels);
    const auto values_per_page = std::size_t{kEnergyPageCells} * kEnergyPageCells * channels;
    const auto max_pages = kMaxRegionalValueBytes / (values_per_page * sizeof(std::uint16_t));
    std::optional<std::int64_t> previous;
    while (std::getline(in, line)) {
        if (staged.m_pages.size() == max_pages)
            return false;
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string tag, x_token, z_token;
        int px = 0, pz = 0;
        if (!(row >> tag >> x_token >> z_token) || tag != "P" ||
            !ParseRegionalInteger(x_token, px) || !ParseRegionalInteger(z_token, pz))
            return false;
        // All physical pages containing legal cell centres lie in this bound.
        // Check before page-to-cell multiplication/allocation.
        if (px < -43691 || px > 43690 || pz < -43691 || pz > 43690)
            return false;
        const auto key = PackPageKey(px, pz);
        if (previous && key <= *previous)
            return false;
        Page page;
        page.values.assign(values_per_page, 0);
        std::optional<std::size_t> prior_cell;
        std::string token;
        while (row >> token) {
            const auto colon = token.find(':');
            std::size_t index = 0;
            unsigned value = 0;
            if (colon == std::string::npos ||
                !ParseRegionalInteger(token.substr(0, colon), index) ||
                !ParseRegionalInteger(token.substr(colon + 1), value) || index >= values_per_page ||
                value == 0 || value > 65535 || (prior_cell && index <= *prior_cell))
                return false;
            const auto cell = index / static_cast<std::size_t>(channels);
            const int cx = px * kEnergyPageCells + static_cast<int>(cell % kEnergyPageCells);
            const int cz = pz * kEnergyPageCells + static_cast<int>(cell / kEnergyPageCells);
            if (!ValidEnergyOwner(OwnerOfCell(cx, cz)))
                return false;
            page.values[index] = static_cast<std::uint16_t>(value);
            prior_cell = index;
        }
        if (!prior_cell)
            return false;
        staged.m_pages.emplace(key, std::move(page));
        previous = key;
    }
    // Exact reencoding rejects alternate whitespace, signed zero, leading zeros,
    // missing newline, duplicate/empty pages and extra/trailing data.
    if (staged.SerializeRegionalRecord() != record)
        return false;
    m_channels = channels;
    m_pages.swap(staged.m_pages);
    m_pending.clear();
    m_fires_completed = 0;
    m_next_fire_tick = kEnergyCadenceTicks;
    return true;
}

bool EnergyFieldState::ValidateRegionalOwners(const RegionRecords& records,
                                              const EnergyAges& ages) const {
    return ValidateRegionalOwners(records, ages, nullptr);
}

bool EnergyFieldState::ValidateRegionalOwners(const RegionRecords& records,
                                              const EnergyAges& ages,
                                              std::vector<std::int64_t>* empty_pages) const {
    if (m_channels < 1 || m_channels > 64 ||
        records.size() > Luminumbra::world::ActiveRegionLedger::kMaxRegions)
        return false;
    for (const auto& [key, age] : ages)
        if (!ValidEnergyOwner(key) || !records.contains(key) ||
            age >= Luminumbra::world::WorldClock::kTickLimit)
            return false;
    for (const auto& [key, page] : m_pages) {
        const auto& facts = page.regional_facts(key, m_channels);
        if (!facts.valid)
            return false;
        for (std::size_t i = 0; i < facts.owner_count; ++i)
            if (!records.contains(facts.owners[i]) || !ages.contains(facts.owners[i]))
                return false;
        if (!facts.any && empty_pages)
            empty_pages->push_back(key);
    }
    return true;
}

bool EnergyFieldState::TickRegional(std::uint64_t tick,
                                    const RegionRecords& records,
                                    std::span<const RegionKey> due,
                                    const EnergyAges& ages,
                                    RegionalResult& out) {
    using Luminumbra::world::RegionState;
    using Luminumbra::world::WorldClock;
    if (tick >= WorldClock::kTickLimit || m_channels < 1 || m_channels > 64 ||
        m_pending.size() > kMaxRegionalDeposits)
        return false;
    RegionalResult result;
    result.ages = ages;
    std::set<RegionKey> firing, age_updates;
    std::optional<RegionKey> previous;
    for (const auto key : due) {
        const auto owner = records.find(key);
        if (!ValidEnergyOwner(key) || owner == records.end() ||
            owner->second.state == RegionState::Frozen || (previous && key <= *previous))
            return false;
        auto [age, added] = result.ages.try_emplace(key, 0);
        (void)added;
        if (age->second >= WorldClock::kTickLimit - 1)
            return false;
        ++age->second;
        age_updates.insert(key);
        if (age->second % kEnergyCadenceTicks == 0)
            firing.insert(key);
        previous = key;
    }
    std::vector<std::int64_t> empty_pages;
    if (!ValidateRegionalOwners(records, result.ages, &empty_pages))
        return false;
    for (const auto& [key, owner] : records)
        if (!ValidEnergyOwner(key) || owner.cadence_shift > 16 ||
            (owner.state != RegionState::Active && owner.state != RegionState::Reduced &&
             owner.state != RegionState::Frozen) ||
            owner.cadence_phase >= (std::uint32_t{1} << owner.cadence_shift))
            return false;

    // Copy only pages that can change. The overlay stays private until all validation passes,
    // so allocation/cap refusal cannot consume deposits or alter dormant authority.
    EnergyFieldState staged(m_channels);
    staged.m_fires_completed = m_fires_completed;
    staged.m_pending = m_pending;
    const auto max_pages =
        kMaxRegionalValueBytes /
        (std::size_t{kEnergyPageCells} * kEnergyPageCells * m_channels * sizeof(std::uint16_t));
    auto stored_pages = m_pages.size();
    if (stored_pages > max_pages)
        return false;
    const auto stage_page = [&](int px, int pz) -> Page& {
        const auto key = PackPageKey(px, pz);
        const auto existing = staged.m_pages.find(key);
        if (existing != staged.m_pages.end()) {
            existing->second.regional_summary.reset();
            return existing->second;
        }
        const auto original = m_pages.find(key);
        if (original != m_pages.end()) {
            auto& page = staged.m_pages.emplace(key, original->second).first->second;
            page.regional_summary.reset();
            return page;
        }
        return staged.EnsurePage(px, pz);
    };
    std::set<std::pair<RegionKey, std::int64_t>> touched;
    std::sort(staged.m_pending.begin(),
              staged.m_pending.end(),
              [](const PendingDeposit& a, const PendingDeposit& b) {
                  return std::tie(a.cell_key, a.channel, a.emitter_id, a.amount) <
                         std::tie(b.cell_key, b.channel, b.emitter_id, b.amount);
              });
    for (const auto& deposit : staged.m_pending) {
        int cx = 0, cz = 0;
        UnpackCellKey(deposit.cell_key, cx, cz);
        const auto owner = OwnerOfCell(cx, cz);
        if (!std::binary_search(due.begin(), due.end(), owner)) {
            result.clipped_raw += deposit.amount;
            continue;
        }
        const int px = FloorDiv(cx, kEnergyPageCells), pz = FloorDiv(cz, kEnergyPageCells);
        const auto page_key = PackPageKey(px, pz);
        if (!staged.m_pages.contains(page_key) && !m_pages.contains(page_key)) {
            if (stored_pages == max_pages)
                return false;
            ++stored_pages;
        }
        auto& page = stage_page(px, pz);
        const auto index = (std::size_t(cz - pz * kEnergyPageCells) * kEnergyPageCells +
                            (cx - px * kEnergyPageCells)) *
                               m_channels +
                           deposit.channel;
        const auto sum = std::uint64_t{page.values[index]} + deposit.amount;
        page.values[index] = static_cast<std::uint16_t>(std::min(sum, std::uint64_t{65535}));
        result.clipped_raw += sum > 65535 ? sum - 65535 : 0;
        touched.emplace(owner, page_key);
    }
    staged.m_pending.clear();

    std::set<std::int64_t> diffusion_pages;
    const auto present = [&](int px, int pz) {
        const auto key = PackPageKey(px, pz);
        return m_pages.contains(key) || staged.m_pages.contains(key);
    };
    const auto include = [&](int px, int pz) {
        if (present(px, pz)) {
            stage_page(px, pz);
            diffusion_pages.insert(PackPageKey(px, pz));
        }
    };
    // Each 512 m owner intersects at most 3 x 3 physical pages. Common borders can fire
    // without an interior due tick, but require both endpoint pages to exist (sealed edges).
    for (const auto owner : firing) {
        const auto [min_x, end_x] = RegionalCellRange(owner.x);
        const auto [min_z, end_z] = RegionalCellRange(owner.z);
        for (int pz = FloorDiv(min_z, kEnergyPageCells);
             pz <= FloorDiv(end_z - 1, kEnergyPageCells);
             ++pz)
            for (int px = FloorDiv(min_x, kEnergyPageCells);
                 px <= FloorDiv(end_x - 1, kEnergyPageCells);
                 ++px)
                include(px, pz);
    }
    for (const auto& [owner, record] : records) {
        if (record.state == RegionState::Frozen ||
            (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) != 0)
            continue;
        const auto [min_x, end_x] = RegionalCellRange(owner.x);
        const auto [min_z, end_z] = RegionalCellRange(owner.z);
        for (const auto& [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}}) {
            const auto neighbor = records.find({owner.x + dx, owner.z + dz});
            if (neighbor == records.end() || neighbor->second.state == RegionState::Frozen)
                continue;
            const auto shift = std::max(record.cadence_shift, neighbor->second.cadence_shift);
            if ((tick & ((std::uint64_t{1} << shift) - 1)) != 0)
                continue;
            const auto lo = dx != 0 ? min_z : min_x;
            const auto end = dx != 0 ? end_z : end_x;
            for (int along = FloorDiv(lo, kEnergyPageCells);
                 along <= FloorDiv(end - 1, kEnergyPageCells);
                 ++along) {
                const int px = dx != 0 ? FloorDiv(end_x - 1, kEnergyPageCells) : along;
                const int pz = dz != 0 ? FloorDiv(end_z - 1, kEnergyPageCells) : along;
                const int npx = dx != 0 ? FloorDiv(end_x, kEnergyPageCells) : px;
                const int npz = dz != 0 ? FloorDiv(end_z, kEnergyPageCells) : pz;
                if (present(px, pz) && present(npx, npz)) {
                    include(px, pz);
                    include(npx, npz);
                }
            }
        }
    }
    result.staged_pages = staged.m_pages.size();
    result.diffusion_pages = diffusion_pages.size();

    // Intrinsic decay precedes the four pinned diffusion sweeps, as in Tick.
    // No page-wide epoch is advanced: cells in the same page can have different owners.
    for (const auto key : diffusion_pages) {
        auto& page = staged.m_pages.at(key);
        int px = 0, pz = 0;
        UnpackCellKey(key, px, pz);
        for (int z = 0; z < kEnergyPageCells; ++z)
            for (int x = 0; x < kEnergyPageCells; ++x) {
                const auto owner =
                    OwnerOfCell(px * kEnergyPageCells + x, pz * kEnergyPageCells + z);
                if (!firing.contains(owner))
                    continue;
                touched.emplace(owner, key);
                for (int channel = 0; channel < m_channels; ++channel) {
                    const auto index =
                        (std::size_t(z) * kEnergyPageCells + x) * m_channels + channel;
                    page.values[index] = DecayOnce(page.values[index]);
                }
            }
    }
    // At most kMaxRegionalValueBytes of u16 pages, and twice that for the reusable i32 buffer.
    // An idle tick allocates no diffusion values. Every sweep reads the same sealed page order.
    std::map<std::int64_t, std::vector<std::int32_t>> next;
    for (const auto key : diffusion_pages)
        next.emplace(key, std::vector<std::int32_t>(staged.m_pages.at(key).values.size()));
    for (int iteration = 0; !next.empty() && iteration < kEnergyDiffuseIterations; ++iteration) {
        // Sealed previous-sweep snapshot. Each undirected edge is processed once;
        // both outflows are subtracted from their source and added to the other.
        // No sweep observes an earlier edge's writes.
        const auto& snapshot = staged.m_pages;
        for (auto& [key, values] : next) {
            const auto& page = snapshot.at(key);
            std::copy(page.values.begin(), page.values.end(), values.begin());
        }
        for (auto& [key, next_values] : next) {
            const auto& page = snapshot.at(key);
            int px = 0, pz = 0;
            UnpackCellKey(key, px, pz);
            for (int z = 0; z < kEnergyPageCells; ++z)
                for (int x = 0; x < kEnergyPageCells; ++x) {
                    const int cx = px * kEnergyPageCells + x, cz = pz * kEnergyPageCells + z;
                    const auto owner_a = OwnerOfCell(cx, cz);
                    const auto a = records.find(owner_a);
                    if (a == records.end() || a->second.state == RegionState::Frozen)
                        continue;
                    for (const auto& [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}}) {
                        const int nx = cx + dx, nz = cz + dz;
                        const auto owner_b = OwnerOfCell(nx, nz);
                        const auto b = records.find(owner_b);
                        if (b == records.end() || b->second.state == RegionState::Frozen)
                            continue;
                        if (owner_a == owner_b) {
                            if (!firing.contains(owner_a))
                                continue;
                        } else {
                            const auto shift =
                                std::max(a->second.cadence_shift, b->second.cadence_shift);
                            if ((tick & ((std::uint64_t{1} << shift) - 1)) != 0)
                                continue;
                        }
                        const int npx = FloorDiv(nx, kEnergyPageCells);
                        const int npz = FloorDiv(nz, kEnergyPageCells);
                        const auto neighbor_key = PackPageKey(npx, npz);
                        const auto neighbor = snapshot.find(neighbor_key);
                        const auto neighbor_next = next.find(neighbor_key);
                        if (neighbor == snapshot.end() || neighbor_next == next.end())
                            continue;
                        const auto base_a = (std::size_t(z) * kEnergyPageCells + x) * m_channels;
                        const auto base_b =
                            (std::size_t(nz - npz * kEnergyPageCells) * kEnergyPageCells +
                             (nx - npx * kEnergyPageCells)) *
                            m_channels;
                        bool processed = false;
                        for (int channel = 0; channel < m_channels; ++channel) {
                            const auto va = page.values[base_a + channel];
                            const auto vb = neighbor->second.values[base_b + channel];
                            if (va == 0 && vb == 0)
                                continue;
                            const auto flux = static_cast<std::int32_t>(va >> kEnergyDiffuseShift) -
                                              static_cast<std::int32_t>(vb >> kEnergyDiffuseShift);
                            next_values[base_a + channel] -= flux;
                            neighbor_next->second[base_b + channel] += flux;
                            processed = true;
                        }
                        if (processed) {
                            // A common border can deliver energy to an owner
                            // before its first interior due tick. Its age starts
                            // at zero; border work never increments active age.
                            if (result.ages.try_emplace(owner_a, 0).second)
                                age_updates.insert(owner_a);
                            if (result.ages.try_emplace(owner_b, 0).second)
                                age_updates.insert(owner_b);
                            touched.emplace(owner_a, key);
                            touched.emplace(owner_b, neighbor_key);
                        }
                    }
                }
        }
        for (const auto& [key, values] : next) {
            auto& page = staged.m_pages.at(key);
            for (std::size_t i = 0; i < values.size(); ++i) {
                // At most four outflows <= v/16, and four inflows <= 65535/16;
                // residue stays in the source, proving exact u16 conservation.
                if (values[i] < 0 || values[i] > 65535)
                    return false;
                page.values[i] = static_cast<std::uint16_t>(values[i]);
            }
        }
    }
    if (!staged.ValidateRegionalOwners(records, result.ages) || !RegionalRecordFits(&staged))
        return false;
    std::map<RegionKey, std::uint64_t> counts;
    for (const auto owner : age_updates)
        ++counts[owner]; // persistent increments and first border acquisitions, even at zero
    for (const auto& [owner, page] : touched) {
        (void)page;
        ++counts[owner];
    }
    for (const auto& [owner, count] : counts)
        result.work.push_back({owner, 0, 0, 0, count});
    for (const auto key : empty_pages)
        m_pages.erase(key);
    while (!staged.m_pages.empty()) {
        auto node = staged.m_pages.extract(staged.m_pages.begin());
        if (node.mapped().all_zero())
            m_pages.erase(node.key());
        else if (const auto original = m_pages.find(node.key()); original != m_pages.end())
            original->second = std::move(node.mapped());
        else
            m_pages.insert(std::move(node));
    }
    m_pending.clear();
    out = std::move(result);
    return true;
}

} // namespace luminumbra::fields
