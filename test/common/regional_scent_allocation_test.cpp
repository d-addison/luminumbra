#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/fields/FieldPageState.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
struct AllocationState {
    bool enabled = false;
    std::size_t calls = 0;
};

AllocationState& Allocations() noexcept {
    static thread_local AllocationState state;
    return state;
}

void RecordAllocation() noexcept {
    auto& state = Allocations();
    if (state.enabled) {
        ++state.calls;
    }
}

class AllocationScope {
public:
    AllocationScope() {
        Allocations() = {true, 0};
    }
    ~AllocationScope() {
        Allocations().enabled = false;
    }
    [[nodiscard]] std::size_t calls() const {
        return Allocations().calls;
    }
};

void FreeAligned(void* storage) noexcept {
#if defined(_WIN32)
    ::_aligned_free(storage);
#else
    std::free(storage);
#endif
}
} // namespace

void* operator new(std::size_t size) {
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
        RecordAllocation();
        return storage;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* storage) noexcept {
    std::free(storage);
}
void operator delete[](void* storage) noexcept {
    ::operator delete(storage);
}
void operator delete(void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* storage = nullptr;
#if defined(_WIN32)
    storage = ::_aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
#else
    if (::posix_memalign(&storage, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) !=
        0) {
        storage = nullptr;
    }
#endif
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    RecordAllocation();
    return storage;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* storage, std::align_val_t) noexcept {
    FreeAligned(storage);
}
void operator delete[](void* storage, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete(void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}

namespace {
using namespace Luminumbra::world;

void CheckCaptureAllocations() {
    FieldPageState live;
    std::array<ScentCell, 4> cells{};
    std::map<RegionKey, std::uint64_t> work;
    cells[1][0] = std::bit_cast<std::uint64_t>(FieldPageState::kScentFloor);
    // Both alternating change buffers and result maps must reach their high-water marks.
    for (int tick = 0; tick < 8; ++tick) {
        cells[0][0] = std::bit_cast<std::uint64_t>(2.0 + tick);
        ASSERT_TRUE(live.capture_scent_window({0, 0}, 2, cells, work));
    }
    for (int tick = 0; tick < 8; ++tick) {
        cells[0][0] = std::bit_cast<std::uint64_t>(20.0 + tick);
        bool accepted = false;
        std::size_t calls = 0;
        {
            const AllocationScope scope;
            accepted = live.capture_scent_window({0, 0}, 2, cells, work);
            calls = scope.calls();
        }
        ASSERT_TRUE(accepted);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(live.scent_at({0, 0}), cells[0]);
        EXPECT_EQ(work, (std::map<RegionKey, std::uint64_t>{{{0, 0}, 1}}));
        EXPECT_EQ(live.scent_capture_visits(), 0u);
    }
}

void CheckCandidateAllocations() {
    FieldPageState live, candidate;
    ScentChangeBuffers buffers;
    ScentWindowWork work;
    std::array<ScentCell, 4> cells{};
    cells[0][0] = std::bit_cast<std::uint64_t>(2.0);
    cells[2][0] = std::bit_cast<std::uint64_t>(FieldPageState::kScentFloor);
    for (int tick = 0; tick < 8; ++tick) {
        cells[1][0] = tick % 2 == 0 ? cells[2][0] : 0;
        candidate.reuse_scent_buffers(buffers);
        ASSERT_TRUE(candidate.stage_scent_window(live, {0, 0}, 2, cells, work));
        EXPECT_EQ(candidate.scent_capture_visits(), cells.size());
        live.adopt_scent_state(candidate);
        candidate.reuse_scent_buffers(buffers);
        cells[0][0] = std::bit_cast<std::uint64_t>(3.0 + tick);
    }
    candidate.reuse_scent_buffers(buffers);
    const auto before = live.serialize();
    cells[0][0] = std::bit_cast<std::uint64_t>(42.0);
    cells[1][0] = cells[2][0];
    ASSERT_TRUE(candidate.stage_scent_window(live, {0, 0}, 2, cells, work));
    const auto staged = candidate.staged_scent_cells();
    const auto saved_work = work.entries;
    auto invalid = cells;
    invalid.back()[0] = std::bit_cast<std::uint64_t>(-1.0);
    bool accepted = true;
    std::size_t calls = 0;
    {
        const AllocationScope scope;
        accepted = candidate.stage_scent_window(live, {0, 0}, 2, invalid, work);
        calls = scope.calls();
    }
    EXPECT_FALSE(accepted);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(candidate.staged_scent_cells(), staged);
    EXPECT_EQ(work.entries, saved_work);
    EXPECT_EQ(live.serialize(), before);
    live.adopt_scent_state(candidate);
    EXPECT_EQ(live.scent_at({0, 0}), cells[0]);
    EXPECT_EQ(live.scent_at({0, 1}), cells[1]);
    EXPECT_EQ(live.scent_at({1, 0}), cells[2]);
    candidate.reuse_scent_buffers(buffers);
    {
        const AllocationScope scope;
        candidate.reuse_scent_buffers(buffers);
        accepted = candidate.stage_scent_window(live, {0, 0}, 2, cells, work);
        live.adopt_scent_state(candidate);
        candidate.reuse_scent_buffers(buffers);
        calls = scope.calls();
    }
    ASSERT_TRUE(accepted);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(work.count, 0u);
}

void CheckChangingWorkOwners() {
    FieldPageState live;
    std::array<ScentCell, 4> cells{};
    std::map<RegionKey, std::uint64_t> work;
    double amount = 2.0;
    for (const auto changed : {3u, 3u, 2u, 2u, 1u, 3u, 0u}) {
        const auto value = std::bit_cast<std::uint64_t>(amount++);
        if ((changed & 1u) != 0) {
            cells[0][0] = value;
        }
        if ((changed & 2u) != 0) {
            cells[2][0] = value;
        }
        ASSERT_TRUE(live.capture_scent_window({511, 0}, 2, cells, work));
        EXPECT_EQ(work.size(), static_cast<std::size_t>((changed & 1u) + ((changed >> 1u) & 1u)));
        EXPECT_EQ(work.contains({0, 0}), (changed & 1u) != 0);
        EXPECT_EQ(work.contains({1, 0}), (changed & 2u) != 0);
        EXPECT_EQ(live.scent_at({511, 0}), cells[0]);
        EXPECT_EQ(live.scent_at({512, 0}), cells[2]);
    }
}

void CheckSolverAllocations(bool diffuse, bool gated, bool advect) {
    luminumbra::ai::ScentField field(8, 8, 4);
    luminumbra::fields::ScalarFieldDiffusion scalar(8, 8);
    const luminumbra::ai::ScentField::CellGate participants =
        gated ? luminumbra::ai::ScentField::CellGate([](int x, int) { return x < 4; })
              : luminumbra::ai::ScentField::CellGate{};
    const luminumbra::ai::ScentField::EdgeGate exchange =
        gated ? luminumbra::ai::ScentField::EdgeGate(
                    [](int x, int, int nx, int) { return (x < 4) == (nx < 4); })
              : luminumbra::ai::ScentField::EdgeGate{};
    const auto iterations = diffuse ? std::size_t{2} : 0;
    const double wind_x = advect ? 0.25 : 0.0;
    const double wind_z = advect ? -0.5 : 0.0;
    for (int channel = 0; channel < field.channels(); ++channel) {
        field.Deposit(channel, 2, 2, 8.0);
    }
    field.Step(0.125, iterations, 0.05, wind_x, wind_z, participants, exchange);
    for (int tick = 0; tick < 8; ++tick) {
        std::size_t scalar_calls = 0;
        if (diffuse) {
            const AllocationScope scope;
            (void)scalar.diffuse(iterations, 0.125);
            scalar_calls = scope.calls();
        }
        std::size_t calls = 0;
        {
            const AllocationScope scope;
            field.Step(0.125, iterations, 0.05, wind_x, wind_z, participants, exchange);
            calls = scope.calls();
        }
        // The scalar solver still allocates its report and iteration buffers. Scent's
        // advection and smoothing snapshots add no requests beyond that public baseline.
        EXPECT_EQ(calls, static_cast<std::size_t>(field.channels()) * scalar_calls);
        if (gated) {
            EXPECT_DOUBLE_EQ(field.Sample(0, 6, 2), 0.0);
        }
    }
}

TEST(RegionalScentAllocation, CaptureReusesStorageAndPreservesRefusedCandidates) {
    ASSERT_NO_FATAL_FAILURE(CheckCaptureAllocations());
    ASSERT_NO_FATAL_FAILURE(CheckCandidateAllocations());
    ASSERT_NO_FATAL_FAILURE(CheckChangingWorkOwners());
}

TEST(RegionalScentAllocation, SolverReusesSnapshotsForEveryChannelAndGatePath) {
    for (const bool diffuse : {false, true}) {
        for (const bool gated : {false, true}) {
            for (const bool advect : {false, true}) {
                ASSERT_NO_FATAL_FAILURE(CheckSolverAllocations(diffuse, gated, advect));
            }
        }
    }
}
} // namespace
