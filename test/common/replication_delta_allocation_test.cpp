#include "net/ReplicationDelta.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
// The standard library also allocates through the nothrow forms (for example
// std::stable_sort's temporary buffer), so they must share the replaced heap.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}
void operator delete(void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}
void operator delete(void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}

namespace {
using namespace Luminumbra::Net;

TEST(ReplicationDeltaAllocations, FlatScratchAndReservedOutputsHaveConstantAllocationCount) {
    // Release libraries allocate once per nonempty vector. MSVC debug iterators
    // also allocate container proxies, so allow one proxy per vector and move.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    constexpr std::size_t kMakeAllocationBudget = 12;
    constexpr std::size_t kApplyAllocationBudget = 15;
#else
    constexpr std::size_t kMakeAllocationBudget = 4;
    constexpr std::size_t kApplyAllocationBudget = 5;
#endif
    for (const std::uint32_t count : {64u, 1024u}) {
        SnapshotMsg baseline;
        SnapshotMsg current;
        current.removed_ids = {(std::numeric_limits<std::uint32_t>::max)(),
                               (std::numeric_limits<std::uint32_t>::max)()};
        for (std::uint32_t id = 0; id < count; ++id) {
            ReplEntityState entity;
            entity.entity_id = id;
            baseline.entities.push_back(entity);
            entity.entity_id += count / 2;
            entity.px_mm = 1;
            current.entities.push_back(entity);
        }
        // Exercise duplicate baseline indexing without another retained entity.
        baseline.entities.push_back(baseline.entities.front());
        for (int tick = 0; tick < 8; ++tick) {
            current.server_tick = static_cast<std::uint64_t>(tick);
            std::size_t make_calls = 0;
            std::size_t apply_calls = 0;
            const auto delta = [&] {
                const AllocationScope scope;
                auto result = MakeSnapshotDelta(baseline, current);
                make_calls = scope.calls();
                return result;
            }();
            const auto full = [&] {
                const AllocationScope scope;
                auto result = TryApplySnapshotDelta(
                    baseline, delta, (std::numeric_limits<std::size_t>::max)());
                apply_calls = scope.calls();
                return result;
            }();
            EXPECT_LE(make_calls, kMakeAllocationBudget);
            EXPECT_LE(apply_calls, kApplyAllocationBudget);
            if (!full) {
                FAIL() << "reconstruction unexpectedly refused unbounded input";
                return;
            }
            EXPECT_EQ(full->entities, current.entities);
            EXPECT_EQ(delta.entities.size(), count);
            EXPECT_EQ(delta.removed_ids.size(), count / 2 + 1);
        }
    }
}
} // namespace
