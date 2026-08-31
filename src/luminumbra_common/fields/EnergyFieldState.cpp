#include "EnergyFieldState.h"

#include <algorithm>
#include <sstream>

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
    for (const std::uint16_t v : values) {
        if (v != 0)
            return false;
    }
    return true;
}

EnergyFieldState::EnergyFieldState(int channels)
    : m_channels(channels < 1 ? 1 : channels) {}

std::int64_t EnergyFieldState::PackPageKey(int px, int pz) noexcept {
    return (static_cast<std::int64_t>(px) << 32) |
           (static_cast<std::int64_t>(static_cast<std::uint32_t>(pz)));
}

std::int64_t EnergyFieldState::PackCellKey(int cx, int cz) noexcept {
    return (static_cast<std::int64_t>(cx) << 32) |
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
    // SEQUENTIAL per-step loop — bit-equal to the in-window path (Codex r2: a
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
        page.values.assign(
            static_cast<std::size_t>(kEnergyPageCells) * kEnergyPageCells * m_channels, 0);
        page.last_step = m_fires_completed;
        it = m_pages.emplace(key, std::move(page)).first;
    }
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
            (static_cast<std::size_t>(lz) * kEnergyPageCells + lx) * m_channels + d.channel;
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
                            page.values[(static_cast<std::size_t>(lz) * kEnergyPageCells + lx) *
                                            m_channels +
                                        ch];
                        if (v == 0)
                            continue;
                        win[static_cast<std::size_t>(pz * kEnergyPageCells + lz) * n +
                            (px * kEnergyPageCells + lx)] = v;
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
                        const std::size_t i = static_cast<std::size_t>(z) * n + x;
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
                                        (px * kEnergyPageCells + lx)] != 0) {
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
                    for (int lz = 0; lz < kEnergyPageCells; ++lz) {
                        for (int lx = 0; lx < kEnergyPageCells; ++lx) {
                            page.values[(static_cast<std::size_t>(lz) * kEnergyPageCells + lx) *
                                            m_channels +
                                        ch] =
                                win[static_cast<std::size_t>(pz * kEnergyPageCells + lz) * n +
                                    (px * kEnergyPageCells + lx)];
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

    std::uint64_t clipped = ApplyDeposits();

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
        .values[(static_cast<std::size_t>(lz) * kEnergyPageCells + lx) * m_channels + channel];
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
        page.values.assign(static_cast<std::size_t>(kEnergyPageCells) * kEnergyPageCells * channels,
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

} // namespace luminumbra::fields
