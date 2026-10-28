#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Core/STLAlignedAllocator.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuadTree.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <gtest/gtest.h>

#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <stdexcept>
#include <string>
#include <utility>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <malloc.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
#endif
#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#endif

namespace {
// Child processes own the global allocation callbacks. This fixed ledger never
// allocates; its 4096 entries bound both live storage and measured sweep length.
struct AllocationLedger {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::array<void*, 4096> blocks{};
    std::atomic<bool> capture{false};
    std::atomic<bool> workers_only{false};
    std::atomic<std::size_t> attempts{0};
    std::atomic<std::size_t> fail_at{0};
    std::atomic<std::size_t> failures{0};
    bool return_null = false;
    bool continue_refusing = false;
    std::thread::id creator{};
    unsigned zero_allocation_calls = 0;
};
AllocationLedger& Allocations() {
    // Warm this nonallocating state before installing callbacks or capturing.
    // It owns only fixed storage and has no destructor work.
    static AllocationLedger state;
    return state;
}

void Require(bool value, int code = 91) {
    if (!value) {
        std::_Exit(code);
    }
}

void Record(void* block) {
    if (block == nullptr || !Allocations().capture.load()) {
        return;
    }
    while (Allocations().lock.test_and_set(std::memory_order_acquire)) {}
    for (void*& slot : Allocations().blocks) {
        if (slot == nullptr) {
            slot = block;
            Allocations().lock.clear(std::memory_order_release);
            return;
        }
    }
    std::_Exit(92);
}

void Forget(void* block) {
    while (Allocations().lock.test_and_set(std::memory_order_acquire)) {}
    for (void*& slot : Allocations().blocks) {
        if (slot == block) {
            slot = nullptr;
            break;
        }
    }
    Allocations().lock.clear(std::memory_order_release);
}

std::size_t Outstanding() {
    std::size_t count = 0;
    for (void* block : Allocations().blocks) {
        count += block != nullptr ? 1U : 0U;
    }
    return count;
}

bool Refuse() {
    if (!Allocations().capture.load() || (Allocations().workers_only.load() &&
                                          std::this_thread::get_id() == Allocations().creator)) {
        return false;
    }
    const std::size_t ordinal = Allocations().attempts.fetch_add(1) + 1;
    const std::size_t selected = Allocations().fail_at.load();
    if (selected == 0 || ordinal < selected ||
        (!Allocations().continue_refusing && ordinal != selected)) {
        return false;
    }
    Allocations().failures.fetch_add(1);
    return true;
}

void Arm(std::size_t ordinal, bool null_result, bool continued, bool workers = false) {
    Allocations().creator = std::this_thread::get_id();
    Allocations().attempts = 0;
    Allocations().failures = 0;
    Allocations().fail_at = ordinal;
    Allocations().return_null = null_result;
    Allocations().continue_refusing = continued;
    Allocations().workers_only = workers;
    Allocations().capture = true;
}

void Disarm() {
    Allocations().capture = false;
    Allocations().fail_at = 0;
    Allocations().workers_only = false;
}

void* AllocateBytes(std::size_t size, std::size_t alignment, bool nullable) {
    if (Refuse()) {
        if (nullable && Allocations().return_null) {
            return nullptr;
        }
        throw std::bad_alloc();
    }
    void* block = nullptr;
    const std::size_t bytes = size == 0 ? 1 : size;
    if (alignment == 0) {
        block = std::malloc(bytes);
    } else {
#if defined(_WIN32)
        block = _aligned_malloc(bytes, alignment);
#else
        if (posix_memalign(&block, alignment, bytes) != 0) {
            block = nullptr;
        }
#endif
    }
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    Record(block);
    return block;
}

void FreeBytes(void* block, bool aligned) noexcept {
    Forget(block);
#if defined(_WIN32)
    if (aligned) {
        _aligned_free(block);
    } else {
        std::free(block);
    }
#else
    static_cast<void>(aligned);
    std::free(block);
#endif
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void* JoltAllocate(std::size_t size) {
    return AllocateBytes(size, 0, true);
}
void JoltFree(void* block) {
    FreeBytes(block, false);
}
void* JoltAlignedAllocate(std::size_t size, std::size_t alignment) {
    return AllocateBytes(size, alignment, true);
}
void JoltAlignedFree(void* block) {
    FreeBytes(block, true);
}
void* JoltReallocate(void* block, std::size_t old_size, std::size_t size) {
    if (Refuse()) {
        if (Allocations().return_null) {
            return nullptr;
        }
        throw std::bad_alloc();
    }
    // Remember the old pointer before realloc invalidates it, preserving its
    // ledger ownership if the real allocator refuses the request.
    void* replacement = std::malloc(size);
    if (replacement == nullptr) {
        throw std::bad_alloc();
    }
    if (block != nullptr) {
        std::memcpy(replacement, block, std::min(old_size, size));
    }
    Forget(block);
    std::free(block);
    Record(replacement);
    return replacement;
}
#endif

void InitializeAllocators() {
    static_cast<void>(Allocations());
    JPH::RegisterDefaultAllocator();
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    JPH::Allocate = JoltAllocate;
    JPH::Reallocate = JoltReallocate;
    JPH::Free = JoltFree;
    JPH::AlignedAllocate = JoltAlignedAllocate;
    JPH::AlignedFree = JoltAlignedFree;
#endif
}

class Layers final : public JPH::BroadPhaseLayerInterface {
public:
    bool throw_name = false;
    mutable unsigned names = 0;
    JPH::uint GetNumBroadPhaseLayers() const override {
        return 2;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        ++names;
        if (throw_name && layer == JPH::BroadPhaseLayer(0)) {
            throw std::bad_alloc();
        }
        return "Layer";
    }
#endif
};
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
        return true;
    }
};
class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override {
        return true;
    }
};

void Startup() {
    Layers layers;
    ObjectVsBroadPhase broad_filter;
    ObjectPairs pair_filter;
    JPH::TempAllocatorImpl temp(10 * 1024 * 1024);
    Require(temp.GetSize() == 10 * 1024 * 1024);
    JPH::PhysicsSystem physics;
    physics.Init(10240, 0, 10240, 10240, layers, broad_filter, pair_filter);
    Require(physics.GetMaxBodies() == 10240);
    JPH::JobSystemThreadPool pool(2048, 8, 0);
    Require(pool.GetMaxConcurrency() == 1);
}

void AllocationSweep() {
    InitializeAllocators();
    // Measure a finite real startup sequence. No ordinal is guessed or silently
    // omitted; every observed allocation is reached again in all four modes.
    Arm(0, false, false);
    Startup();
    const std::size_t measured = Allocations().attempts.load();
    Disarm();
    Require(measured > 0 && measured < Allocations().blocks.size());
    Require(Outstanding() == 0);
    for (bool null_result : {false, true}) {
        for (bool continued : {false, true}) {
            for (std::size_t ordinal = 1; ordinal <= measured; ++ordinal) {
                bool caught = false;
                Arm(ordinal, null_result, continued);
                try {
                    Startup();
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
                const bool reached = Allocations().failures.load() != 0;
                Disarm();
                Require(caught && reached && Outstanding() == 0);
            }
        }
    }
    std::_Exit(0);
}

struct CopyRefusal {};
struct Callable {
    static inline bool refuse_copy = false;
    static inline int live = 0;
    Callable() {
        ++live;
    }
    Callable(const Callable&) {
        if (refuse_copy) {
            throw CopyRefusal{};
        }
        ++live;
    }
    ~Callable() {
        --live;
    }
    void operator()() const {}
};

template<class Pool>
void CopyFailures(Pool& pool) {
    const JPH::JobSystem::JobFunction function = Callable{};
    const int baseline = Callable::live;
    // First-unused and recycled storage both encounter the actual JobFunction
    // copy. The next allocation must recover the single slot in either case.
    for (int attempt = 0; attempt < 2; ++attempt) {
        Callable::refuse_copy = true;
        bool caught = false;
        try {
            const auto job = pool.CreateJob("copy", JPH::Color::sGreen, function, 1);
            static_cast<void>(job);
        } catch (const CopyRefusal&) {
            caught = true;
        }
        Callable::refuse_copy = false;
        Require(caught && Callable::live == baseline);
        auto job = pool.CreateJob("reused", JPH::Color::sGreen, function, 1);
        Require(job.IsValid());
        auto* barrier = pool.CreateBarrier();
        Require(barrier != nullptr);
        barrier->AddJob(job);
        job.RemoveDependency();
        pool.WaitForJobs(barrier);
        Require(job.IsDone());
        pool.DestroyBarrier(barrier);
    }
}

void JobCopyFailures() {
    InitializeAllocators();
    {
        JPH::JobSystemThreadPool pool(1, 8, 0);
        CopyFailures(pool);
    }
    {
        JPH::JobSystemSingleThreaded pool(1);
        CopyFailures(pool);
    }
    Require(Callable::live == 0);
    std::_Exit(0);
}

void DefaultAndPartialLayers() {
    InitializeAllocators();
    {
        JPH::QuadTree tree;
        JPH::BroadPhaseQuadTree broad_phase;
        JPH::PhysicsSystem physics;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    Arm(0, false, false);
    bool caught = false;
    Layers layers;
    layers.throw_name = true;
    ObjectVsBroadPhase broad_filter;
    ObjectPairs pair_filter;
    try {
        JPH::PhysicsSystem physics;
        physics.Init(10240, 0, 10240, 10240, layers, broad_filter, pair_filter);
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    Disarm();
    Require(caught && layers.names == 1 && Outstanding() == 0);
#endif
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void UnbackedPage() {
    InitializeAllocators();
    {
        JPH::FixedSizeFreeList<int> slots;
        slots.Init(256, 256);
        Arm(1, true, true);
        bool caught = false;
        try {
            static_cast<void>(slots.ConstructObject(7));
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        Require(caught && Allocations().failures.load() == 1);
        // Destruction occurs while all further acquisitions are still refused.
    }
    Disarm();
    Require(Outstanding() == 0);
    std::_Exit(0);
}

#endif

#if defined(__linux__)
void BeginNativeAccounting();
void EndNativeAccounting(unsigned expected);
#endif

void WorkerCallbacks() {
    InitializeAllocators();
#if defined(__linux__)
    BeginNativeAccounting();
#endif
    std::atomic<unsigned> initialized{0};
    std::atomic<unsigned> exited{0};
    bool caught = false;
    {
        JPH::JobSystemThreadPool pool;
        pool.SetThreadInitFunction([&initialized](int index) {
            if (index == 1) {
                throw CopyRefusal{};
            }
            ++initialized;
        });
        pool.SetThreadExitFunction([&exited](int) noexcept { ++exited; });
        try {
            pool.Init(2048, 8, 3);
        } catch (const CopyRefusal&) {
            caught = true;
        }
        Require(caught && pool.GetMaxConcurrency() == 1);
    }
    Require(initialized.load() == 2 && exited.load() == 2);
#if defined(__linux__)
    EndNativeAccounting(3);
#endif
    std::_Exit(0);
}

#if defined(JPH_PROFILE_ENABLED) && !defined(JPH_EXTERNAL_PROFILE)
void ProfileStartup() {
    InitializeAllocators();
    // Measure the real worker's ProfileThread and profiler registration path.
    std::size_t measured = 0;
    for (std::size_t ordinal = 0; ordinal <= measured; ++ordinal) {
#if defined(__linux__)
        BeginNativeAccounting();
#endif
        bool caught = false;
        {
            JPH::Profiler profiler;
            JPH::Profiler::sInstance = &profiler;
            Arm(ordinal, true, true, true);
            try {
                JPH::JobSystemThreadPool pool(2048, 8, 1);
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            if (ordinal == 0) {
                measured = Allocations().attempts.load();
                Require(measured > 0 && measured < Allocations().blocks.size());
            } else {
                Require(caught && Allocations().failures.load() != 0);
            }
            Disarm();
            JPH::Profiler::sInstance = nullptr;
        }
        Require(Outstanding() == 0);
#if defined(__linux__)
        EndNativeAccounting(1);
#endif
    }
    std::_Exit(0);
}
#endif

void PopulatedTeardown() {
    InitializeAllocators();
    JPH::Factory factory;
    JPH::Factory::sInstance = &factory;
    JPH::RegisterTypes();
    Layers layers;
    ObjectVsBroadPhase broad_filter;
    ObjectPairs pair_filter;
    JPH::TempAllocatorImpl temp(10 * 1024 * 1024);
    JPH::JobSystemSingleThreaded jobs(2048);
    // Factory/types, temporary storage and jobs are unrelated owners outside
    // capture. Update(0) takes the broadphase-only path without creating jobs.
    Require(Outstanding() == 0);
    Arm(0, false, false);
    const auto recorded = [](const void* block) {
        return std::count(Allocations().blocks.begin(), Allocations().blocks.end(), block) == 1;
    };
    auto* physics = new JPH::PhysicsSystem;
    Require(recorded(physics));
    physics->Init(10240, 0, 10240, 10240, layers, broad_filter, pair_filter);
    const std::size_t initialized_blocks = Outstanding();
    Require(initialized_blocks > 1);
    auto& bodies = physics->GetBodyInterface();
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5F));
    const JPH::Shape* owned_shape = shape.GetPtr();
    Require(recorded(owned_shape) && Outstanding() == initialized_blocks + 1);
    std::array<JPH::BodyID, 512> ids{};
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto x = static_cast<float>(index);
        ids[index] = bodies.CreateAndAddBody(
            JPH::BodyCreationSettings(
                shape, JPH::RVec3(x, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0),
            JPH::EActivation::DontActivate);
        Require(!ids[index].IsInvalid());
    }
    physics->OptimizeBroadPhase();
    // A selective rebuild retains unchanged subtrees and retires a disjoint
    // old-node batch. Both must be destroyed once without following old parents.
    bodies.SetPosition(ids[0], JPH::RVec3(-2, 0, 0), JPH::EActivation::DontActivate);
    static_cast<void>(physics->Update(0.0F, 1, &temp, &jobs));
    Require(physics->GetNumBodies() == ids.size());
    for (const auto id : ids) {
        const JPH::BodyLockRead body_lock(physics->GetBodyLockInterface(), id);
        Require(body_lock.SucceededAndIsInBroadPhase());
        const auto& body = body_lock.GetBody();
        Require(recorded(&body) && body.GetShape() == owned_shape);
    }
    Require(owned_shape->GetRefCount() == ids.size() + 1);
    // Drop the external reference while all 512 bodies still own the shape.
    shape = nullptr;
    Require(owned_shape->GetRefCount() == ids.size() && recorded(owned_shape));
    const std::size_t acquired_blocks = Outstanding();
    Require(acquired_blocks >= initialized_blocks + ids.size() + 1);
    Arm(1, false, true);
    // Switching to refusal resets attempt counters, never the acquired roster.
    Require(Outstanding() == acquired_blocks && recorded(physics) && recorded(owned_shape));
    delete physics;
    const std::size_t teardown_allocations = Allocations().attempts.load();
    Disarm();
    Require(teardown_allocations == 0 && Outstanding() == 0);
    JPH::UnregisterTypes();
    JPH::Factory::sInstance = nullptr;
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void FactoryGrowth() {
    InitializeAllocators();
    const auto destruct = +[](void* object) {
        delete static_cast<unsigned char*>(object);
    };
    JPH::RTTI sentinel("Sentinel", 1, nullptr, destruct);
    JPH::RTTI extra("Extra", 1, nullptr, destruct);
    const JPH::RTTI* additions[128];
    for (auto& entry : additions) {
        entry = &extra;
    }
    // Bulk registration reserves hash then name maps. Measure and fail both
    // actual replacement allocations while preserving the populated entries.
    std::size_t measured = 0;
    for (std::size_t ordinal = 0; ordinal <= measured; ++ordinal) {
        {
            JPH::Factory factory;
            Require(factory.Register(&sentinel));
            Arm(ordinal, true, true);
            bool caught = false;
            try {
                Require(factory.Register(additions, 128));
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            if (ordinal == 0) {
                measured = Allocations().attempts.load();
                Require(measured > 0 && measured < Allocations().blocks.size());
                // Each map reserves for 129 entries; the 128 additions share
                // one type, so insertion needs no allocation after both reserves.
                Require(measured == 2);
                Require(factory.Find(extra.GetName()) == &extra);
                Require(factory.Find(extra.GetHash()) == &extra);
            } else {
                Require(caught && Allocations().failures.load() != 0);
                Require(factory.Find(extra.GetName()) == nullptr);
                Require(factory.Find(extra.GetHash()) == nullptr);
            }
            Disarm();
            Require(factory.Find(sentinel.GetName()) == &sentinel);
            Require(factory.Find(sentinel.GetHash()) == &sentinel);
        }
        Require(Outstanding() == 0);
    }
    std::_Exit(0);
}
#endif
} // namespace

// Test-executable-only allocation siblings cover native new[] and the build
// configuration where Jolt deliberately disables its custom allocator hooks.
void* operator new(std::size_t size) {
    return AllocateBytes(size, 0, false);
}
void* operator new[](std::size_t size) {
    return AllocateBytes(size, 0, false);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return AllocateBytes(size, static_cast<std::size_t>(alignment), false);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return AllocateBytes(size, static_cast<std::size_t>(alignment), false);
}
void operator delete(void* block) noexcept {
    FreeBytes(block, false);
}
void operator delete[](void* block) noexcept {
    FreeBytes(block, false);
}
void operator delete(void* block, std::size_t) noexcept {
    FreeBytes(block, false);
}
void operator delete[](void* block, std::size_t) noexcept {
    FreeBytes(block, false);
}
void operator delete(void* block, std::align_val_t) noexcept {
    FreeBytes(block, true);
}
void operator delete[](void* block, std::align_val_t) noexcept {
    FreeBytes(block, true);
}
void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    FreeBytes(block, true);
}
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept {
    FreeBytes(block, true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* block, const std::nothrow_t&) noexcept {
    FreeBytes(block, false);
}
void operator delete[](void* block, const std::nothrow_t&) noexcept {
    FreeBytes(block, false);
}
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    FreeBytes(block, true);
}
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept {
    FreeBytes(block, true);
}

TEST(JoltStartup, MeasuredNullThrowAndUnwindSweep) {
    ASSERT_EXIT(AllocationSweep(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, DefaultAndMixedLayerDestruction) {
    ASSERT_EXIT(DefaultAndPartialLayers(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, BackedCallableCopyRestoresBothJobConsumers) {
    ASSERT_EXIT(JobCopyFailures(), ::testing::ExitedWithCode(0), "");
}
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
TEST(JoltStartup, UnbackedPageFailureIsDiscardOnly) {
    ASSERT_EXIT(UnbackedPage(), ::testing::ExitedWithCode(0), "");
}
#endif
TEST(JoltStartup, WorkerInitializationPropagatesAndCleansAcquiredSetup) {
    ASSERT_EXIT(WorkerCallbacks(), ::testing::ExitedWithCode(0), "");
}
#if defined(JPH_PROFILE_ENABLED) && !defined(JPH_EXTERNAL_PROFILE)
TEST(JoltStartup, RealWorkerProfilerFailuresReachCreator) {
    ASSERT_EXIT(ProfileStartup(), ::testing::ExitedWithCode(0), "");
}
#endif
TEST(JoltStartup, PopulatedRetiredAndRetainedNodesNeedNoTeardownAllocation) {
    ASSERT_EXIT(PopulatedTeardown(), ::testing::ExitedWithCode(0), "");
}
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
TEST(JoltStartup, PopulatedFactoryKeepsEntriesOnReplacementFailure) {
    ASSERT_EXIT(FactoryGrowth(), ::testing::ExitedWithCode(0), "");
}
#endif

#if defined(__linux__)
namespace {
using CreateThread = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
using JoinThread = int (*)(pthread_t, void**);
using InitSemaphore = int (*)(sem_t*, int, unsigned);
using DestroySemaphore = int (*)(sem_t*);
struct NativeLedger {
    std::atomic<unsigned> launch_attempts{0};
    std::atomic<unsigned> launch_failure{0};
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> joined{0};
    unsigned semaphore_attempts = 0;
    unsigned semaphore_failure = 0;
    unsigned semaphore_acquired = 0;
    unsigned semaphore_closed = 0;
    // Fixed storage bounds both acquired handles and every ownership scan.
    std::array<sem_t*, 32> semaphores{};
    bool active = false;
    CreateThread real_create = nullptr;
    JoinThread real_join = nullptr;
    InitSemaphore real_sem_init = nullptr;
    DestroySemaphore real_sem_destroy = nullptr;
};
NativeLedger& Native() {
    // Constant initialization cannot enter allocation or interposed functions.
    // Resolve symbols separately, after this state is available to wrappers.
    static constinit NativeLedger state;
    return state;
}

void ResolveNative() {
    auto& state = Native();
    state.real_create = reinterpret_cast<CreateThread>(dlsym(RTLD_NEXT, "pthread_create"));
    state.real_join = reinterpret_cast<JoinThread>(dlsym(RTLD_NEXT, "pthread_join"));
    state.real_sem_init = reinterpret_cast<InitSemaphore>(dlsym(RTLD_NEXT, "sem_init"));
    state.real_sem_destroy = reinterpret_cast<DestroySemaphore>(dlsym(RTLD_NEXT, "sem_destroy"));
    Require(state.real_create && state.real_join && state.real_sem_init && state.real_sem_destroy);
}

void ResetNative() {
    Native().launch_attempts = 0;
    Native().launch_failure = 0;
    Native().created = 0;
    Native().joined = 0;
    Native().semaphore_attempts = 0;
    Native().semaphore_failure = 0;
    Native().semaphore_acquired = 0;
    Native().semaphore_closed = 0;
    for (auto& semaphore : Native().semaphores) {
        semaphore = nullptr;
    }
    Native().active = true;
}

void BeginNativeAccounting() {
    ResolveNative();
    ResetNative();
}
void EndNativeAccounting(unsigned expected) {
    Require(Native().created.load() == expected && Native().joined.load() == expected);
    Require(Native().semaphore_acquired == Native().semaphore_closed);
    Native().active = false;
}

void ThreadLaunchFailures() {
    InitializeAllocators();
    ResolveNative();
    for (unsigned failure : {1U, 2U}) {
        ResetNative();
        Native().launch_failure = failure;
        Arm(0, false, false);
        bool caught = false;
        try {
            JPH::JobSystemThreadPool pool(2048, 8, 3);
        } catch (const std::system_error& error) {
            caught = error.code().value() == EAGAIN;
        }
        Disarm();
        Require(caught && Native().launch_attempts == failure);
        Require(Native().created == failure - 1 && Native().joined == Native().created);
        Require(Native().semaphore_acquired == Native().semaphore_closed && Outstanding() == 0);
        Native().active = false;
    }
    // Failed replacement stops the old pool and owns no replacement worker.
    ResetNative();
    {
        JPH::JobSystemThreadPool pool(2048, 8, 1);
        Native().launch_failure = Native().launch_attempts.load() + 1;
        bool caught = false;
        try {
            pool.SetNumThreads(3);
        } catch (const std::system_error& error) {
            caught = error.code().value() == EAGAIN;
        }
        Require(caught && pool.GetMaxConcurrency() == 1);
        Require(Native().joined == Native().created);
    }
    Require(Native().semaphore_acquired == Native().semaphore_closed);
    Native().active = false;
    std::_Exit(0);
}

void SemaphoreFailures() {
    InitializeAllocators();
    ResolveNative();
    // One pool semaphore and eight real BarrierImpl semaphore members.
    ResetNative();
    { JPH::JobSystemThreadPool pool(2048, 8, 0); }
    const unsigned measured = Native().semaphore_attempts;
    Require(measured > 1 && Native().semaphore_acquired == Native().semaphore_closed);
    Native().active = false;
    for (unsigned failure = 1; failure <= measured; ++failure) {
        ResetNative();
        Native().semaphore_failure = failure;
        Arm(0, false, false);
        bool caught = false;
        try {
            JPH::JobSystemThreadPool pool(2048, 8, 0);
        } catch (const std::system_error& error) {
            caught = error.code().value() == ENOSPC;
        }
        Disarm();
        Require(caught && Native().semaphore_attempts == failure);
        Require(Native().semaphore_acquired == failure - 1);
        Require(Native().semaphore_closed == Native().semaphore_acquired && Outstanding() == 0);
        Native().active = false;
    }
    std::_Exit(0);
}
} // namespace

// ELF executable interposition reaches libstdc++'s actual pthread_create call;
// wrapping only the test's undefined references would miss that DSO boundary.
extern "C" int pthread_create(pthread_t* thread,
                              const pthread_attr_t* attributes,
                              void* (*entry)(void*),
                              void* argument) noexcept {
    if (Native().real_create == nullptr) {
        ResolveNative();
    }
    if (Native().active) {
        const unsigned attempt = Native().launch_attempts.fetch_add(1) + 1;
        if (attempt == Native().launch_failure.load()) {
            return EAGAIN;
        }
    }
    const int result = Native().real_create(thread, attributes, entry, argument);
    if (Native().active && result == 0) {
        Native().created.fetch_add(1);
    }
    return result;
}
extern "C" int pthread_join(pthread_t thread, void** result) {
    if (Native().real_join == nullptr) {
        ResolveNative();
    }
    const int status = Native().real_join(thread, result);
    if (Native().active && status == 0) {
        Native().joined.fetch_add(1);
    }
    return status;
}
extern "C" int sem_init(sem_t* semaphore, int shared, unsigned value) noexcept {
    if (Native().real_sem_init == nullptr) {
        ResolveNative();
    }
    if (Native().active && ++Native().semaphore_attempts == Native().semaphore_failure) {
        errno = ENOSPC;
        return -1;
    }
    const int result = Native().real_sem_init(semaphore, shared, value);
    if (Native().active && result == 0) {
        Require(Native().semaphore_acquired < Native().semaphores.size());
        Native().semaphores[Native().semaphore_acquired++] = semaphore;
    }
    return result;
}
extern "C" int sem_destroy(sem_t* semaphore) noexcept {
    if (Native().real_sem_destroy == nullptr) {
        ResolveNative();
    }
    if (Native().active) {
        bool found = false;
        for (auto& owned : Native().semaphores) {
            if (owned == semaphore) {
                Require(!found);
                owned = nullptr;
                found = true;
            }
        }
        Require(found);
    }
    const int result = Native().real_sem_destroy(semaphore);
    if (Native().active && result == 0) {
        ++Native().semaphore_closed;
    }
    return result;
}

TEST(JoltStartup, NativeFirstLaterLaunchFailureJoinsEveryCreatedWorker) {
    ASSERT_EXIT(ThreadLaunchFailures(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, NativeSemaphoreFailureClosesEveryAcquiredHandle) {
    ASSERT_EXIT(SemaphoreFailures(), ::testing::ExitedWithCode(0), "");
}
#endif

namespace {
struct PlainAllocation {
    JPH_OVERRIDE_NEW_DELETE
    unsigned value = 7;
};
constexpr std::size_t kAllocationAlignment = 64;
static_assert(sizeof(unsigned) < kAllocationAlignment);
struct alignas(kAllocationAlignment) AlignedAllocation {
    JPH_OVERRIDE_NEW_DELETE
    unsigned value = 7;
    // Explicit storage fills the remainder of this fixed 64-byte allocation.
    std::array<unsigned char, kAllocationAlignment - sizeof(unsigned)> trailing_storage{};
};
static_assert(alignof(AlignedAllocation) == kAllocationAlignment);
static_assert(sizeof(AlignedAllocation) == kAllocationAlignment);
static_assert(offsetof(AlignedAllocation, value) == 0);
static_assert(offsetof(AlignedAllocation, trailing_storage) == sizeof(unsigned));
static_assert(offsetof(AlignedAllocation, trailing_storage) +
                  sizeof(AlignedAllocation::trailing_storage) ==
              kAllocationAlignment);
static_assert(AlignedAllocation{}.value == 7);

template<class Operation>
void RefuseFirstAllocation(Operation operation) {
    for (bool null_result : {false, true}) {
        Arm(1, null_result, true);
        bool caught = false;
        try {
            operation();
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        Disarm();
        Require(caught && Allocations().failures.load() != 0 && Outstanding() == 0);
    }
}

template<class Operation>
void RejectOverflowBeforeAllocation(const Operation& operation) {
    Arm(0, false, false);
    bool caught = false;
    try {
        operation();
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    const auto attempts = Allocations().attempts.load();
    Disarm();
    Require(caught && attempts == 0 && Outstanding() == 0);
}

void OverflowInputs() {
    InitializeAllocators();
    RejectOverflowBeforeAllocation([] {
        JPH::FixedSizeFreeList<int> slots;
        slots.Init(std::numeric_limits<JPH::uint>::max(), 256);
    });
    RejectOverflowBeforeAllocation([] {
        JPH::UnorderedMap<JPH::uint32, const JPH::RTTI*> entries;
        entries.reserve(std::numeric_limits<JPH::uint32>::max());
    });
    RejectOverflowBeforeAllocation([] {
        JPH::STLAllocator<JPH::uint64> allocator;
        static_cast<void>(allocator.allocate(std::numeric_limits<std::size_t>::max()));
    });
    RejectOverflowBeforeAllocation([] {
        JPH::STLAlignedAllocator<JPH::uint64, 64> allocator;
        static_cast<void>(allocator.allocate(std::numeric_limits<std::size_t>::max()));
    });
    std::_Exit(0);
}

void AllocationSiblings() {
    InitializeAllocators();
    RefuseFirstAllocation([] { delete new PlainAllocation; });
    RefuseFirstAllocation([] { delete[] new PlainAllocation[2]; });
    RefuseFirstAllocation([] { delete new AlignedAllocation; });
    RefuseFirstAllocation([] { delete[] new AlignedAllocation[2]; });
    RefuseFirstAllocation([] {
        JPH::STLAllocator<unsigned> allocator;
        auto* block = allocator.allocate(4);
        allocator.deallocate(block, 4);
    });
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    RefuseFirstAllocation([] {
        JPH::STLAllocator<AlignedAllocation> allocator;
        auto* block = allocator.allocate(4);
        allocator.deallocate(block, 4);
    });
    RefuseFirstAllocation([] {
        JPH::STLAlignedAllocator<unsigned, 64> allocator;
        auto* block = allocator.allocate(4);
        allocator.deallocate(block, 4);
    });
    JPH::STLAllocator<unsigned char> allocator;
    auto* original = allocator.allocate(4);
    original[0] = 17;
    Arm(1, true, true);
    bool caught = false;
    try {
        static_cast<void>(allocator.reallocate(original, 4, 8));
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    Disarm();
    Require(caught && original[0] == 17);
    allocator.deallocate(original, 4);
#endif
    bool overflow = false;
    try {
        static_cast<void>(JPH::CheckedMultiply(std::numeric_limits<std::size_t>::max(), 2));
    } catch (const std::bad_alloc&) {
        overflow = true;
    }
    Require(overflow);
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void SharedRootPage() {
    InitializeAllocators();
    {
        JPH::QuadTree::Allocator allocator;
        allocator.Init(10240, 256);
        Arm(0, false, false);
        {
            JPH::QuadTree first;
            JPH::QuadTree second;
            first.Init(allocator);
            second.Init(allocator);
            Require(Allocations().attempts.load() == 1);
        }
        Disarm();
    }
    Require(Outstanding() == 0);
    std::_Exit(0);
}
#endif
} // namespace

TEST(JoltStartup, ScalarArrayAlignedAndReallocationSiblingsRefuseSafely) {
    ASSERT_EXIT(AllocationSiblings(), ::testing::ExitedWithCode(0), "");
}
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
TEST(JoltStartup, TwoNormalLayerRootsShareOnePageOf256Nodes) {
    ASSERT_EXIT(SharedRootPage(), ::testing::ExitedWithCode(0), "");
}
#endif

namespace {
void PoolStorageSweep() {
    InitializeAllocators();
    Arm(0, false, false);
    { JPH::JobSystemThreadPool pool(2048, 8, 1); }
    const std::size_t measured = Allocations().attempts.load();
    Disarm();
    Require(measured > 0 && measured < Allocations().blocks.size() && Outstanding() == 0);
    for (bool null_result : {false, true}) {
        for (bool continued : {false, true}) {
            for (std::size_t ordinal = 1; ordinal <= measured; ++ordinal) {
                Arm(ordinal, null_result, continued);
                bool caught = false;
                try {
                    JPH::JobSystemThreadPool pool(2048, 8, 1);
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
                Disarm();
                Require(caught && Allocations().failures.load() != 0 && Outstanding() == 0);
            }
        }
    }
    std::_Exit(0);
}

struct CaptureLifetime {
    std::atomic<unsigned>* destroyed;
    explicit CaptureLifetime(std::atomic<unsigned>* counter)
        : destroyed(counter) {}
    ~CaptureLifetime() {
        destroyed->fetch_add(1);
    }
};
void QueueCaptureRelease() {
    InitializeAllocators();
    std::atomic<unsigned> destroyed{0};
    {
        JPH::JobSystemThreadPool pool(2048, 8, 2);
        auto capture = std::make_shared<CaptureLifetime>(&destroyed);
        JPH::JobSystem::JobFunction function = [capture] {
        };
        auto job = pool.CreateJob("capture", JPH::Color::sGreen, function, 1);
        auto* barrier = pool.CreateBarrier();
        Require(barrier != nullptr);
        barrier->AddJob(job);
        job.RemoveDependency();
        pool.WaitForJobs(barrier);
        pool.DestroyBarrier(barrier);
        job = JPH::JobSystem::JobHandle();
        function = {};
        capture.reset();
        // Queue-owned references can survive barrier completion; pool shutdown
        // must join and release them before their dependent state is destroyed.
    }
    Require(destroyed.load() == 1);
    std::_Exit(0);
}
} // namespace

TEST(JoltStartup, MeasuredHeadsSlotsAndThreadControlAllocationFailures) {
    ASSERT_EXIT(PoolStorageSweep(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, PoolJoinReleasesRealQueueOwnedCaptures) {
    ASSERT_EXIT(QueueCaptureRelease(), ::testing::ExitedWithCode(0), "");
}

TEST(JoltStartup, OverflowRefusesBeforeAnyAllocationOrPublication) {
    ASSERT_EXIT(OverflowInputs(), ::testing::ExitedWithCode(0), "");
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
namespace {
void* NullOnlyForZeroAllocation(std::size_t size) {
    if (size == 0) {
        ++Allocations().zero_allocation_calls;
        return nullptr;
    }
    return JoltAllocate(size);
}

void FreeNonNullStorage(void* block) {
    Require(block != nullptr);
    JoltFree(block);
}

void ZeroWorkerPool() {
    InitializeAllocators();
    JPH::Allocate = NullOnlyForZeroAllocation;
    JPH::Free = FreeNonNullStorage;
    // Prove the callback's zero-size contract before exercising the real pool.
    Require(JPH::Allocate(0) == nullptr && Allocations().zero_allocation_calls == 1);
    Allocations().zero_allocation_calls = 0;
    unsigned initialized = 0;
    unsigned exited = 0;
    unsigned executed = 0;
    Arm(0, false, false);
    { const JPH::JobSystemThreadPool empty{}; }
    { JPH::JobSystemThreadPool constructed(2048, 8, 0); }
    Require(Outstanding() == 0 && Allocations().zero_allocation_calls == 0);
    {
        JPH::JobSystemThreadPool pool;
        pool.SetThreadInitFunction([&initialized](int) { ++initialized; });
        pool.SetThreadExitFunction([&exited](int) { ++exited; });
        pool.Init(2048, 8, 0);
        Require(pool.GetMaxConcurrency() == 1 && Outstanding() > 0);
        auto run_job = [&] {
            auto job =
                pool.CreateJob("zero worker", JPH::Color::sGreen, [&executed] { ++executed; }, 1);
            auto* barrier = pool.CreateBarrier();
            Require(barrier != nullptr);
            barrier->AddJob(job);
            job.RemoveDependency();
            pool.WaitForJobs(barrier);
            pool.DestroyBarrier(barrier);
        };
        run_job();
        pool.SetNumThreads(0);
        Require(pool.GetMaxConcurrency() == 1);
        run_job();
        Require(executed == 2 && initialized == 0 && exited == 0 &&
                Allocations().zero_allocation_calls == 0);
    }
    Disarm();
    Require(Outstanding() == 0 && initialized == 0 && exited == 0 &&
            Allocations().zero_allocation_calls == 0);
    Require(JPH::Allocate == NullOnlyForZeroAllocation);
    Require(JPH::Free == FreeNonNullStorage);
    JPH::Allocate = JoltAllocate;
    JPH::Free = JoltFree;
    std::_Exit(0);
}
} // namespace

TEST(JoltStartup, ZeroWorkersAcceptNullOnlyForZeroAllocatorAndExecuteBarrierJobs) {
    ASSERT_EXIT(ZeroWorkerPool(), ::testing::ExitedWithCode(0), "");
}
#endif

namespace {
using ApplicationPhysics = Luminumbra::Systems::PhysicsSystem;

#if defined(_WIN32) && !defined(JPH_DISABLE_CUSTOM_ALLOCATOR)
// Patch resolved imports in this child process, including the CRT DLL that
// implements std::thread. Executable-only replacement misses that DLL boundary.
struct WindowsNativeState {
    const DWORD owner = GetCurrentThreadId();
    bool active = false;
    unsigned launches = 0;
    unsigned fail_launch = 0;
    unsigned semaphores = 0;
    unsigned fail_semaphore = 0;
    unsigned failures = 0;
    unsigned created = 0;
    unsigned joined = 0;
    unsigned acquired = 0;
    unsigned closed = 0;
    struct OwnedHandle {
        HANDLE handle = nullptr;
        bool worker = false;
    };
    // Bound every ownership scan and refuse roster overflow explicitly.
    std::array<OwnedHandle, 256> handles{};
    decltype(&CreateThread) create_thread = nullptr;
    decltype(&CreateSemaphoreA) create_semaphore_a = nullptr;
    decltype(&CreateSemaphoreW) create_semaphore_w = nullptr;
    decltype(&CloseHandle) close_handle = nullptr;
};
WindowsNativeState& WindowsNative() {
    static WindowsNativeState state;
    return state;
}
bool CaptureWindowsNative() {
    const auto& state = WindowsNative();
    return GetCurrentThreadId() == state.owner && state.active;
}
void RecordWindowsHandle(HANDLE handle, bool worker) {
    if (handle == nullptr || !CaptureWindowsNative()) {
        return;
    }
    auto& state = WindowsNative();
    for (auto& slot : state.handles) {
        if (slot.handle == nullptr) {
            slot = {handle, worker};
            if (worker) {
                ++state.created;
            } else {
                ++state.acquired;
            }
            return;
        }
    }
    Require(false);
}
bool RefuseWindowsCreation(bool worker) {
    if (!CaptureWindowsNative()) {
        return false;
    }
    auto& state = WindowsNative();
    const unsigned attempt = worker ? ++state.launches : ++state.semaphores;
    if (attempt != (worker ? state.fail_launch : state.fail_semaphore)) {
        return false;
    }
    ++state.failures;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return true;
}
HANDLE WINAPI StartupCreateThread(LPSECURITY_ATTRIBUTES attributes,
                                  SIZE_T stack_size,
                                  LPTHREAD_START_ROUTINE entry,
                                  LPVOID argument,
                                  DWORD flags,
                                  LPDWORD identifier) {
    if (RefuseWindowsCreation(true)) {
        return nullptr;
    }
    HANDLE handle =
        WindowsNative().create_thread(attributes, stack_size, entry, argument, flags, identifier);
    RecordWindowsHandle(handle, true);
    return handle;
}
HANDLE WINAPI StartupCreateSemaphoreA(LPSECURITY_ATTRIBUTES attributes,
                                      LONG initial,
                                      LONG maximum,
                                      LPCSTR name) {
    if (RefuseWindowsCreation(false)) {
        return nullptr;
    }
    HANDLE handle = WindowsNative().create_semaphore_a(attributes, initial, maximum, name);
    RecordWindowsHandle(handle, false);
    return handle;
}
HANDLE WINAPI StartupCreateSemaphoreW(LPSECURITY_ATTRIBUTES attributes,
                                      LONG initial,
                                      LONG maximum,
                                      LPCWSTR name) {
    if (RefuseWindowsCreation(false)) {
        return nullptr;
    }
    HANDLE handle = WindowsNative().create_semaphore_w(attributes, initial, maximum, name);
    RecordWindowsHandle(handle, false);
    return handle;
}
BOOL WINAPI StartupCloseHandle(HANDLE handle) {
    auto& state = WindowsNative();
    if (CaptureWindowsNative()) {
        for (auto& slot : state.handles) {
            if (slot.handle == handle) {
                if (slot.worker) {
                    // A closed worker must already have terminated, including
                    // when startup fails after earlier successful launches.
                    Require(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0);
                }
                const BOOL result = state.close_handle(handle);
                Require(result != FALSE);
                if (slot.worker) {
                    ++state.joined;
                } else {
                    ++state.closed;
                }
                slot = {};
                return result;
            }
        }
    }
    return state.close_handle(handle);
}

template<class Function>
Function ResolveWindowsFunction(HMODULE module, const char* name) {
    const FARPROC address = GetProcAddress(module, name);
    Require(address != nullptr);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

class ScopedWindowsImports {
public:
    ScopedWindowsImports() {
        auto& state = WindowsNative();
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        Require(kernel != nullptr);
        state.create_thread =
            ResolveWindowsFunction<decltype(state.create_thread)>(kernel, "CreateThread");
        state.create_semaphore_a =
            ResolveWindowsFunction<decltype(state.create_semaphore_a)>(kernel, "CreateSemaphoreA");
        state.create_semaphore_w =
            ResolveWindowsFunction<decltype(state.create_semaphore_w)>(kernel, "CreateSemaphoreW");
        state.close_handle =
            ResolveWindowsFunction<decltype(state.close_handle)>(kernel, "CloseHandle");
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        Require(snapshot != INVALID_HANDLE_VALUE);
        MODULEENTRY32W module{};
        module.dwSize = static_cast<DWORD>(sizeof(module));
        Require(Module32FirstW(snapshot, &module) != FALSE);
        unsigned modules = 0;
        do {
            // Bound module traversal as well as the total patched import slots.
            Require(++modules <= 256);
            PatchModule(module.hModule);
        } while (Module32NextW(snapshot, &module) != FALSE);
        Require(GetLastError() == ERROR_NO_MORE_FILES);
        Require(state.close_handle(snapshot) != FALSE && m_count != 0);
    }
    ~ScopedWindowsImports() {
        Require(!WindowsNative().active);
        while (m_count != 0) {
            const auto& patch = m_patches[--m_count];
            Replace(patch.slot, patch.original);
        }
    }
    ScopedWindowsImports(const ScopedWindowsImports&) = delete;
    ScopedWindowsImports& operator=(const ScopedWindowsImports&) = delete;

private:
    struct ImportPatch {
        ULONG_PTR* slot = nullptr;
        ULONG_PTR original = 0;
    };
    // Fixed storage bounds installation and restoration; no hook allocates.
    std::array<ImportPatch, 1024> m_patches{};
    std::size_t m_count = 0;
    // Cap total descriptor and thunk visits across all loaded modules.
    std::size_t m_scanned = 0;

    static void Replace(ULONG_PTR* slot, ULONG_PTR replacement) {
        DWORD prior = 0;
        Require(VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &prior) != FALSE);
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot),
                                   reinterpret_cast<PVOID>(replacement));
        DWORD ignored = 0;
        Require(VirtualProtect(slot, sizeof(*slot), prior, &ignored) != FALSE);
    }
    void PatchModule(HMODULE module) {
        auto* base = reinterpret_cast<unsigned char*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        Require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        Require(nt->Signature == IMAGE_NT_SIGNATURE);
        const DWORD image_size = nt->OptionalHeader.SizeOfImage;
        const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress == 0) {
            return;
        }
        Require(imports.VirtualAddress < image_size &&
                imports.Size <= image_size - imports.VirtualAddress);
        const auto* descriptors =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
        // Loader-validated images still get explicit image-size bounds on scans.
        for (std::size_t index = 0; index < imports.Size / sizeof(*descriptors); ++index) {
            Require(++m_scanned <= 262144);
            const auto& descriptor = descriptors[index];
            if (descriptor.Name == 0) {
                return;
            }
            Require(descriptor.FirstThunk < image_size);
            auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor.FirstThunk);
            const std::size_t count = (image_size - descriptor.FirstThunk) / sizeof(*thunks);
            for (std::size_t entry = 0; entry < count && thunks[entry].u1.Function != 0; ++entry) {
                Require(++m_scanned <= 262144);
                auto* slot = reinterpret_cast<ULONG_PTR*>(&thunks[entry].u1.Function);
                const auto& state = WindowsNative();
                ULONG_PTR replacement = 0;
                if (*slot == reinterpret_cast<ULONG_PTR>(state.create_thread)) {
                    replacement = reinterpret_cast<ULONG_PTR>(&StartupCreateThread);
                } else if (module == GetModuleHandleW(nullptr) &&
                           *slot == reinterpret_cast<ULONG_PTR>(state.create_semaphore_a)) {
                    replacement = reinterpret_cast<ULONG_PTR>(&StartupCreateSemaphoreA);
                } else if (module == GetModuleHandleW(nullptr) &&
                           *slot == reinterpret_cast<ULONG_PTR>(state.create_semaphore_w)) {
                    replacement = reinterpret_cast<ULONG_PTR>(&StartupCreateSemaphoreW);
                } else if (*slot == reinterpret_cast<ULONG_PTR>(state.close_handle)) {
                    replacement = reinterpret_cast<ULONG_PTR>(&StartupCloseHandle);
                }
                if (replacement != 0) {
                    Require(m_count < m_patches.size());
                    m_patches[m_count++] = {slot, *slot};
                    Replace(slot, replacement);
                }
            }
        }
        Require(false);
    }
};

void BeginWindowsAccounting(unsigned fail_launch = 0, unsigned fail_semaphore = 0) {
    auto& state = WindowsNative();
    Require(!state.active && state.created == state.joined && state.acquired == state.closed);
    state.launches = 0;
    state.fail_launch = fail_launch;
    state.semaphores = 0;
    state.fail_semaphore = fail_semaphore;
    state.failures = 0;
    state.created = 0;
    state.joined = 0;
    state.acquired = 0;
    state.closed = 0;
    state.active = true;
}
void EndWindowsAccounting() {
    auto& state = WindowsNative();
    Require(state.created == state.joined && state.acquired == state.closed);
    for (const auto& slot : state.handles) {
        Require(slot.handle == nullptr);
    }
    state.active = false;
}

void WindowsPoolLaunchFailures() {
    InitializeAllocators();
    {
        ScopedWindowsImports imports;
        // Three requested workers make partial creation reachable even on a
        // single-core host, independently of application CPU auto-detection.
        for (unsigned failure : {1U, 2U}) {
            BeginWindowsAccounting(failure);
            Arm(0, false, false);
            bool caught = false;
            try {
                JPH::JobSystemThreadPool pool(2048, 8, 3);
            } catch (const std::system_error&) {
                caught = true;
            }
            Disarm();
            Require(caught && WindowsNative().failures == 1 &&
                    WindowsNative().launches == failure && WindowsNative().created == failure - 1);
            EndWindowsAccounting();
            Require(Outstanding() == 0);
        }
    }
    std::_Exit(0);
}
#endif

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
struct RuntimeAllocationState {
    // Exclusive child-process hook custody. The fixed 4096-entry roster bounds
    // every diagnostic scan; production allocation and scheduling are unchanged.
    std::array<void*, 4096> blocks{};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::thread::id owner = std::this_thread::get_id();
    JPH::AllocateFunction allocate = JPH::Allocate;
    JPH::ReallocateFunction reallocate = JPH::Reallocate;
    JPH::FreeFunction free = JPH::Free;
    JPH::AlignedAllocateFunction aligned_allocate = JPH::AlignedAllocate;
    JPH::AlignedFreeFunction aligned_free = JPH::AlignedFree;
    std::size_t ordinal = 0;
    std::size_t fail_at = 0;
    bool null_result = false;
    bool material_failure = false;
    bool failed = false;
    void* physics = nullptr;
    void* temp = nullptr;
    unsigned physics_acquisitions = 0;
    unsigned temp_acquisitions = 0;
    unsigned dependency_releases = 0;
    bool observe = false;
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
RuntimeAllocationState* runtime_allocations = nullptr;

bool RefuseRuntimeAllocation() {
    auto& state = *runtime_allocations;
    if (std::this_thread::get_id() != state.owner) {
        return false;
    }
    ++state.ordinal;
    const bool material_ready = state.material_failure && JPH::Factory::sInstance != nullptr &&
                                JPH::Factory::sInstance->Find("PhysicsMaterialSimple") != nullptr &&
                                JPH::PhysicsMaterial::sDefault == nullptr;
    if (!state.failed &&
        ((state.fail_at != 0 && state.ordinal == state.fail_at) || material_ready)) {
        state.failed = true;
        if (state.null_result) {
            return true;
        }
        throw std::bad_alloc();
    }
    return false;
}
void RecordRuntimeBlock(void* block, std::size_t size) {
    if (block == nullptr) {
        return;
    }
    auto& state = *runtime_allocations;
    while (state.lock.test_and_set(std::memory_order_acquire)) {}
    bool recorded = false;
    for (void*& slot : state.blocks) {
        if (slot == nullptr) {
            slot = block;
            recorded = true;
            break;
        }
    }
    state.lock.clear(std::memory_order_release);
    Require(recorded);
    if (state.observe && std::this_thread::get_id() == state.owner) {
        if (size == sizeof(JPH::PhysicsSystem)) {
            Require(state.physics == nullptr);
            state.physics = block;
            ++state.physics_acquisitions;
        }
        if (size == 10 * 1024 * 1024) {
            Require(state.temp == nullptr);
            state.temp = block;
            ++state.temp_acquisitions;
        }
    }
}
void ForgetRuntimeBlock(void* block) {
    if (block == nullptr) {
        return;
    }
    auto& state = *runtime_allocations;
    if (std::this_thread::get_id() == state.owner &&
        (block == state.physics || block == state.temp)) {
#if defined(__linux__)
        // These are actual allocations returned to the shipping constructor.
        // Native interposition observes real successful pthread_join returns,
        // including on rollback from logging after the pool has been built.
        Require(Native().active);
        Require(Native().created.load() == Native().joined.load());
#endif
#if defined(_WIN32)
        if (CaptureWindowsNative()) {
            Require(WindowsNative().created == WindowsNative().joined);
        }
#endif
        Require(JPH::Factory::sInstance != nullptr);
        if (block == state.physics) {
            state.physics = nullptr;
        } else {
            Require(state.physics == nullptr);
            state.temp = nullptr;
        }
        ++state.dependency_releases;
    }
    while (state.lock.test_and_set(std::memory_order_acquire)) {}
    bool found = false;
    for (void*& slot : state.blocks) {
        if (slot == block) {
            Require(!found);
            slot = nullptr;
            found = true;
        }
    }
    state.lock.clear(std::memory_order_release);
    Require(found);
}
void* RuntimeAllocate(std::size_t size) {
    if (RefuseRuntimeAllocation()) {
        return nullptr;
    }
    void* block = runtime_allocations->allocate(size);
    RecordRuntimeBlock(block, size);
    return block;
}
void* RuntimeAlignedAllocate(std::size_t size, std::size_t alignment) {
    if (RefuseRuntimeAllocation()) {
        return nullptr;
    }
    void* block = runtime_allocations->aligned_allocate(size, alignment);
    RecordRuntimeBlock(block, size);
    return block;
}
void RuntimeFree(void* block) {
    ForgetRuntimeBlock(block);
    runtime_allocations->free(block);
}
void RuntimeAlignedFree(void* block) {
    ForgetRuntimeBlock(block);
    runtime_allocations->aligned_free(block);
}
void* RuntimeReallocate(void* block, std::size_t old_size, std::size_t new_size) {
    if (RefuseRuntimeAllocation()) {
        return nullptr;
    }
    // Retain the old allocation when the supported callback refuses.
    void* replacement = runtime_allocations->allocate(new_size);
    if (replacement == nullptr) {
        return nullptr;
    }
    if (block != nullptr) {
        std::memcpy(replacement, block, (std::min)(old_size, new_size));
        RuntimeFree(block);
    }
    RecordRuntimeBlock(replacement, new_size);
    return replacement;
}

class ScopedRuntimeHooks {
public:
    ScopedRuntimeHooks() {
        Require(runtime_allocations == nullptr);
        runtime_allocations = &state;
        JPH::Allocate = RuntimeAllocate;
        JPH::Reallocate = RuntimeReallocate;
        JPH::Free = RuntimeFree;
        JPH::AlignedAllocate = RuntimeAlignedAllocate;
        JPH::AlignedFree = RuntimeAlignedFree;
#if defined(__linux__)
        BeginNativeAccounting();
#endif
    }
    ~ScopedRuntimeHooks() {
        Require(state.physics == nullptr && state.temp == nullptr);
#if defined(__linux__)
        EndNativeAccounting(Native().created.load());
#endif
        // Delegated storage, including process-static RTTI, stays compatible
        // with these saved callbacks. No callback is restored to null.
        JPH::Allocate = state.allocate;
        JPH::Reallocate = state.reallocate;
        JPH::Free = state.free;
        JPH::AlignedAllocate = state.aligned_allocate;
        JPH::AlignedFree = state.aligned_free;
        runtime_allocations = nullptr;
    }
    ScopedRuntimeHooks(const ScopedRuntimeHooks&) = delete;
    ScopedRuntimeHooks& operator=(const ScopedRuntimeHooks&) = delete;

    void ArmFailure(std::size_t ordinal, bool return_null, bool material = false) {
        state.ordinal = 0;
        state.fail_at = ordinal;
        state.null_result = return_null;
        state.material_failure = material;
        state.failed = false;
    }
    void DisarmFailure() {
        state.fail_at = 0;
        state.material_failure = false;
    }
    void RequireInstalled() const {
        Require(JPH::Allocate == RuntimeAllocate && JPH::Reallocate == RuntimeReallocate &&
                JPH::Free == RuntimeFree && JPH::AlignedAllocate == RuntimeAlignedAllocate &&
                JPH::AlignedFree == RuntimeAlignedFree);
    }
    void RequireRetained(const std::array<void*, 4096>& retained) const {
        for (void* block : retained) {
            if (block != nullptr) {
                Require(std::count(state.blocks.begin(), state.blocks.end(), block) == 1);
            }
        }
    }
    RuntimeAllocationState state;
};
#endif

class RefusingStartupSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    unsigned refusals = 0;

protected:
    void sink_it_(const spdlog::details::log_msg&) override {
        ++refusals;
        throw std::bad_alloc();
    }
    void flush_() override {}
};
class ScopedStartupLogRefusal {
public:
    ScopedStartupLogRefusal()
        : m_prior(Log::GetCoreLogger())
        , m_sink(std::make_shared<RefusingStartupSink>()) {
        auto logger = std::make_shared<spdlog::logger>("physics-startup-refusal", m_sink);
        logger->set_level(spdlog::level::info);
        logger->set_error_handler([](const std::string&) { throw std::bad_alloc(); });
        Log::GetCoreLogger() = std::move(logger);
    }
    ~ScopedStartupLogRefusal() {
        Log::GetCoreLogger() = std::move(m_prior);
    }
    void RequireRefused() const {
        Require(m_sink->refusals == 1);
    }
    ScopedStartupLogRefusal(const ScopedStartupLogRefusal&) = delete;
    ScopedStartupLogRefusal& operator=(const ScopedStartupLogRefusal&) = delete;

private:
    std::shared_ptr<spdlog::logger> m_prior;
    std::shared_ptr<RefusingStartupSink> m_sink;
};

void RequireUnpublished(ApplicationPhysics& physics) {
    Require(!physics.is_started());
    Require(physics.create_dynamic_sphere({0, 5, 0}, {0, 0, 0}, 0.25F).IsInvalid());
    physics.update(0.0F);
    physics.shutdown();
}

#if defined(_WIN32) && !defined(JPH_DISABLE_CUSTOM_ALLOCATOR)
void WindowsApplicationCreationFailures(bool workers) {
    Log::Init();
    InitializeAllocators();
    {
        ScopedWindowsImports imports;
        ScopedRuntimeHooks hooks;
        hooks.state.observe = true;
        ApplicationPhysics physics;
        // Warm process-static RTTI before retaining the allocation baseline. Only
        // per-attempt resources must disappear; registration keeps static storage.
        BeginWindowsAccounting();
        physics.startup();
        Require(physics.is_started());
        physics.shutdown();
        EndWindowsAccounting();
        const auto baseline = hooks.state.blocks;
        const unsigned measured = workers ? WindowsNative().launches : WindowsNative().semaphores;
        Require(measured > 0 && measured <= WindowsNative().handles.size());
        const unsigned attempts = workers ? 2 : measured;
        for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
            // First and last worker refusals cover empty and partially built pools
            // without repeating startup for every CPU on large Windows hosts.
            const unsigned failure = workers && attempt == 2 ? measured : attempt;
            Arm(0, false, false);
            BeginWindowsAccounting(workers ? failure : 0, workers ? 0 : failure);
            bool caught = false;
            try {
                physics.startup();
            } catch (const std::system_error& error) {
                // Thread CRTs translate the Win32 error to their own errno. Jolt's
                // semaphore constructor preserves the original system error code.
                caught = workers || (error.code().category() == std::system_category() &&
                                     error.code().value() == ERROR_NOT_ENOUGH_MEMORY);
            }
            Require(caught && WindowsNative().failures == 1);
            Require((workers ? WindowsNative().launches : WindowsNative().semaphores) == failure);
            if (workers) {
                Require(WindowsNative().created == failure - 1);
            } else {
                Require(WindowsNative().acquired == failure - 1 && WindowsNative().created == 0);
            }
            RequireUnpublished(physics);
            EndWindowsAccounting();
            Disarm();
            Require(Outstanding() == 0);
            Require(JPH::Factory::sInstance == nullptr &&
                    JPH::PhysicsMaterial::sDefault == nullptr);
            Require(hooks.state.physics == nullptr && hooks.state.temp == nullptr &&
                    hooks.state.blocks == baseline);
            hooks.RequireInstalled();
            // The same object must reacquire the released lease and publish a
            // complete runtime after each distinct native creation refusal.
            Arm(0, false, false);
            BeginWindowsAccounting();
            physics.startup();
            Require(physics.is_started());
            physics.shutdown();
            EndWindowsAccounting();
            Disarm();
            Require(Outstanding() == 0);
            Require(JPH::Factory::sInstance == nullptr &&
                    JPH::PhysicsMaterial::sDefault == nullptr && hooks.state.blocks == baseline);
        }
    }
    std::_Exit(0);
}
#endif

// Initialize the real logger before installing hooks or arming any refusal.
// Log and the registry retain its console and periodic workers across all
// physics-only accounting windows, including temporary logger replacement.
void RepeatedApplicationLifecycle() {
    Log::Init();
    InitializeAllocators();
    auto* const prior_factory = JPH::Factory::sInstance;
    const auto prior_material = JPH::PhysicsMaterial::sDefault;
    {
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
        ScopedRuntimeHooks hooks;
        hooks.state.observe = true;
#endif
        ApplicationPhysics physics;
        RequireUnpublished(physics);
        for (int cycle = 0; cycle < 2; ++cycle) {
            physics.startup();
            Require(physics.is_started() && JPH::Factory::sInstance != nullptr);
            auto* const factory = JPH::Factory::sInstance;
            const auto material = JPH::PhysicsMaterial::sDefault;
            physics.startup();
            Require(JPH::Factory::sInstance == factory &&
                    JPH::PhysicsMaterial::sDefault == material);
            physics.create_player_controller({0, 4, 0});
            Require(physics.create_avatar_character({2, 4, 0}) == 0);
            physics.update_avatars(1.0F / 60.0F);
            Require(physics.get_avatar_position(0).x == 2.0F);
            physics.set_avatar_wish_velocity(0, {5.0F, 0.0F});
            const auto body = physics.create_dynamic_sphere({0, 8, 0}, {0, 0, 0}, 0.25F);
            Require(!body.IsInvalid());
            physics.update(1.0F / 60.0F);
            physics.shutdown();
            Require(!physics.is_started() && physics.avatar_character_count() == 0);
            Require(JPH::Factory::sInstance == prior_factory &&
                    JPH::PhysicsMaterial::sDefault == prior_material);
            RequireUnpublished(physics);
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
            hooks.RequireInstalled();
#endif
        }
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
        Require(hooks.state.physics_acquisitions == 2 && hooks.state.temp_acquisitions == 2 &&
                hooks.state.dependency_releases == 4);
#endif
    }
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void ColdApplicationRefusal(std::size_t ordinal, bool null_result, bool material_failure) {
    Log::Init();
    InitializeAllocators();
    {
        ScopedRuntimeHooks hooks;
        hooks.ArmFailure(ordinal, null_result, material_failure);
        ApplicationPhysics physics;
        bool caught = false;
        try {
            physics.startup();
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        hooks.DisarmFailure();
        Require(caught && hooks.state.failed && JPH::Factory::sInstance == nullptr &&
                JPH::PhysicsMaterial::sDefault == nullptr);
        RequireUnpublished(physics);
        hooks.RequireInstalled();
        physics.startup();
        Require(physics.is_started());
        physics.shutdown();
        Require(JPH::Factory::sInstance == nullptr && JPH::PhysicsMaterial::sDefault == nullptr);
    }
    Require(JPH::Allocate == JoltAllocate && JPH::Reallocate == JoltReallocate &&
            JPH::Free == JoltFree && JPH::AlignedAllocate == JoltAlignedAllocate &&
            JPH::AlignedFree == JoltAlignedFree);
    std::_Exit(0);
}

void PriorMaterialAndPartialCallbacks() {
    Log::Init();
    InitializeAllocators();
    ApplicationPhysics physics;
    // Every incomplete callback sibling refuses before global mutation.
    for (unsigned missing = 0; missing < 5; ++missing) {
        const auto allocate = JPH::Allocate;
        const auto reallocate = JPH::Reallocate;
        const auto free = JPH::Free;
        const auto aligned_allocate = JPH::AlignedAllocate;
        const auto aligned_free = JPH::AlignedFree;
        if (missing == 0)
            JPH::Allocate = nullptr;
        if (missing == 1)
            JPH::Reallocate = nullptr;
        if (missing == 2)
            JPH::Free = nullptr;
        if (missing == 3)
            JPH::AlignedAllocate = nullptr;
        if (missing == 4)
            JPH::AlignedFree = nullptr;
        bool refused = false;
        try {
            physics.startup();
        } catch (const std::logic_error&) {
            refused = true;
        }
        Require(refused && JPH::Factory::sInstance == nullptr &&
                JPH::PhysicsMaterial::sDefault == nullptr);
        Require(JPH::Allocate == (missing == 0 ? nullptr : allocate) &&
                JPH::Reallocate == (missing == 1 ? nullptr : reallocate) &&
                JPH::Free == (missing == 2 ? nullptr : free) &&
                JPH::AlignedAllocate == (missing == 3 ? nullptr : aligned_allocate) &&
                JPH::AlignedFree == (missing == 4 ? nullptr : aligned_free));
        JPH::Allocate = allocate;
        JPH::Reallocate = reallocate;
        JPH::Free = free;
        JPH::AlignedAllocate = aligned_allocate;
        JPH::AlignedFree = aligned_free;
        RequireUnpublished(physics);
    }
    {
        ScopedRuntimeHooks hooks;
        const JPH::RefConst<JPH::PhysicsMaterial> prior =
            new JPH::PhysicsMaterialSimple("retained", JPH::Color::sGreen);
        JPH::PhysicsMaterial::sDefault = prior;
        hooks.ArmFailure(2, true);
        bool caught = false;
        try {
            physics.startup();
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        hooks.DisarmFailure();
        Require(caught && hooks.state.failed && JPH::Factory::sInstance == nullptr &&
                JPH::PhysicsMaterial::sDefault == prior);
        physics.startup();
        physics.shutdown();
        Require(JPH::Factory::sInstance == nullptr && JPH::PhysicsMaterial::sDefault == prior);
        JPH::PhysicsMaterial::sDefault = nullptr;
    }
    std::_Exit(0);
}

void BeginCandidateAccounting() {
#if defined(__linux__)
    BeginNativeAccounting();
#endif
}
void EndCandidateAccounting() {
#if defined(__linux__)
    EndNativeAccounting(Native().created.load());
#endif
}

void RetainedApplicationOwner(bool borrowed, bool null_result, bool log_failure) {
    Log::Init();
    InitializeAllocators();
    {
        ScopedRuntimeHooks hooks;
        JPH::RTTI sentinel("ExternalSentinel", 1, nullptr, [](void*) {});
        std::unique_ptr<JPH::Factory> external;
        if (borrowed) {
            external = std::make_unique<JPH::Factory>();
            JPH::Factory::sInstance = external.get();
            JPH::RegisterTypes();
            Require(external->Register(&sentinel));
        }
#if defined(__linux__)
        // Only the candidate attempt owns native accounting. The retained
        // owner's idle handles are outside each bounded attempt roster.
        Native().active = false;
#endif
        ApplicationPhysics old;
        old.startup();
        auto* const factory = JPH::Factory::sInstance;
        const auto* const type = factory->Find("PhysicsMaterialSimple");
        const auto material = JPH::PhysicsMaterial::sDefault;
        const auto body = old.create_dynamic_sphere({0, 8, 0}, {0, 0, 0}, 0.25F);
        old.create_player_controller({0, 4, 0});
        Require(!body.IsInvalid());
        const auto position = old.get_body_position(body);
        const auto player = old.get_player_position();
        const auto retained = hooks.state.blocks;

        hooks.state.observe = true;
        ApplicationPhysics candidate;
        const auto require_old = [&] {
            RequireUnpublished(candidate);
            hooks.RequireInstalled();
            hooks.RequireRetained(retained);
            Require(old.is_started() && JPH::Factory::sInstance == factory &&
                    factory->Find("PhysicsMaterialSimple") == type &&
                    JPH::PhysicsMaterial::sDefault == material);
            Require(old.get_body_position(body) == position && old.get_player_position() == player);
        };
        if (log_failure) {
            bool caught = false;
            {
                ScopedStartupLogRefusal refusal;
                BeginCandidateAccounting();
                try {
                    candidate.startup();
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
                refusal.RequireRefused();
            }
            EndCandidateAccounting();
            Require(caught);
            require_old();
        } else {
            hooks.ArmFailure(0, false);
            BeginCandidateAccounting();
            candidate.startup();
            const std::size_t measured = hooks.state.ordinal;
            hooks.DisarmFailure();
            candidate.shutdown();
            EndCandidateAccounting();
            Require(measured > 4 && measured < hooks.state.blocks.size());
            require_old();
            // Measure the real creator-thread callback sequence, including pool
            // construction. Workers forward through the same stable callbacks
            // but never consume failure ordinals. No hooks change while live.
            for (std::size_t selected = 1; selected <= measured; ++selected) {
                hooks.ArmFailure(selected, null_result);
                BeginCandidateAccounting();
                bool caught = false;
                try {
                    candidate.startup();
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
                hooks.DisarmFailure();
                EndCandidateAccounting();
                Require(caught && hooks.state.failed);
                require_old();
            }
        }
        BeginCandidateAccounting();
        candidate.startup();
        Require(candidate.is_started() && JPH::Factory::sInstance == factory);
        candidate.shutdown();
        EndCandidateAccounting();
        hooks.RequireRetained(retained);
        Require(hooks.state.physics == nullptr && hooks.state.temp == nullptr);
        hooks.state.observe = false;
        old.update(1.0F / 60.0F);
        Require(old.get_body_position(body).y < position.y);
        old.shutdown();
        if (borrowed) {
            Require(JPH::Factory::sInstance == external.get() &&
                    external->Find("PhysicsMaterialSimple") == type &&
                    external->Find("ExternalSentinel") == &sentinel &&
                    JPH::PhysicsMaterial::sDefault == material);
            JPH::UnregisterTypes();
            JPH::Factory::sInstance = nullptr;
            external.reset();
        } else {
            Require(JPH::Factory::sInstance == nullptr &&
                    JPH::PhysicsMaterial::sDefault == nullptr);
        }
    }
    std::_Exit(0);
}
#endif

void ApplicationExternalFactoryRefusal() {
    Log::Init();
    InitializeAllocators();
    JPH::Factory unrelated;
    JPH::Factory::sInstance = &unrelated;
    ApplicationPhysics candidate;
    bool refused = false;
    try {
        candidate.startup();
    } catch (const std::logic_error&) {
        refused = true;
    }
    Require(refused && JPH::Factory::sInstance == &unrelated &&
            JPH::PhysicsMaterial::sDefault == nullptr &&
            unrelated.Find("PhysicsMaterialSimple") == nullptr);
    RequireUnpublished(candidate);
    JPH::Factory::sInstance = nullptr;
    ApplicationPhysics old;
    old.startup();
    auto* const factory = JPH::Factory::sInstance;
    const auto material = JPH::PhysicsMaterial::sDefault;
    // No updates or producers overlap this owner-thread identity refusal.
    // Idle workers do not read the factory; restore it before retiring either.
    JPH::Factory::sInstance = &unrelated;
    refused = false;
    try {
        candidate.startup();
    } catch (const std::logic_error&) {
        refused = true;
    }
    Require(refused && JPH::Factory::sInstance == &unrelated &&
            JPH::PhysicsMaterial::sDefault == material);
    JPH::Factory::sInstance = factory;
    RequireUnpublished(candidate);
    candidate.startup();
    candidate.shutdown();
    Require(old.is_started() && JPH::Factory::sInstance == factory);
    old.shutdown();
    Require(JPH::Factory::sInstance == nullptr);
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void ApplicationChangedAllocatorCallbacksRefusal() {
    Log::Init();
    InitializeAllocators();
    ApplicationPhysics physics;
    physics.startup();
    Require(physics.is_started() && JPH::Factory::sInstance != nullptr);
    physics.shutdown();
    Require(JPH::Factory::sInstance == nullptr && JPH::PhysicsMaterial::sDefault == nullptr);
    {
        ScopedRuntimeHooks hooks;
        bool refused = false;
        try {
            physics.startup();
        } catch (const std::logic_error&) {
            refused = true;
        }
        Require(refused && JPH::Factory::sInstance == nullptr &&
                JPH::PhysicsMaterial::sDefault == nullptr);
        hooks.RequireInstalled();
        Require(hooks.state.ordinal == 0);
        RequireUnpublished(physics);
    }
    Require(JPH::Allocate == JoltAllocate && JPH::Reallocate == JoltReallocate &&
            JPH::Free == JoltFree && JPH::AlignedAllocate == JoltAlignedAllocate &&
            JPH::AlignedFree == JoltAlignedFree);
    physics.startup();
    Require(physics.is_started() && JPH::Factory::sInstance != nullptr);
    physics.shutdown();
    Require(JPH::Factory::sInstance == nullptr && JPH::PhysicsMaterial::sDefault == nullptr);
    std::_Exit(0);
}

void ApplicationDefaultAllocatorLifetime() {
    Log::Init();
    Require(JPH::Allocate == nullptr && JPH::Reallocate == nullptr && JPH::Free == nullptr &&
            JPH::AlignedAllocate == nullptr && JPH::AlignedFree == nullptr);
    ApplicationPhysics physics;
    physics.startup();
    const auto allocate = JPH::Allocate;
    const auto reallocate = JPH::Reallocate;
    const auto free = JPH::Free;
    const auto aligned_allocate = JPH::AlignedAllocate;
    const auto aligned_free = JPH::AlignedFree;
    physics.shutdown();
    Require(allocate && reallocate && free && aligned_allocate && aligned_free);
    Require(JPH::Allocate == allocate && JPH::Reallocate == reallocate && JPH::Free == free &&
            JPH::AlignedAllocate == aligned_allocate && JPH::AlignedFree == aligned_free);
    // All runtime resources and workers are gone. Clearing callbacks after
    // process-static RTTI use is an external inconsistency, never cold startup.
    JPH::Allocate = nullptr;
    JPH::Reallocate = nullptr;
    JPH::Free = nullptr;
    JPH::AlignedAllocate = nullptr;
    JPH::AlignedFree = nullptr;
    bool refused = false;
    try {
        physics.startup();
    } catch (const std::logic_error&) {
        refused = true;
    }
    Require(refused && JPH::Factory::sInstance == nullptr && JPH::Allocate == nullptr &&
            JPH::Reallocate == nullptr && JPH::Free == nullptr && JPH::AlignedAllocate == nullptr &&
            JPH::AlignedFree == nullptr);
    JPH::Allocate = allocate;
    JPH::Reallocate = reallocate;
    JPH::Free = free;
    JPH::AlignedAllocate = aligned_allocate;
    JPH::AlignedFree = aligned_free;
    RequireUnpublished(physics);
    physics.startup();
    physics.shutdown();
    Require(JPH::Factory::sInstance == nullptr);
    std::_Exit(0);
}
#endif

void ApplicationLeaseStorageRefusal() {
    Log::Init();
    InitializeAllocators();
    ApplicationPhysics physics;
    Arm(1, false, false);
    bool caught = false;
    try {
        physics.startup();
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    Disarm();
    Require(caught && Allocations().failures == 1 && Outstanding() == 0 &&
            JPH::Factory::sInstance == nullptr);
    RequireUnpublished(physics);
    physics.startup();
    physics.shutdown();
    Require(JPH::Factory::sInstance == nullptr);
    std::_Exit(0);
}

void ApplicationLogRollbackAndDestructor() {
    Log::Init();
    InitializeAllocators();
    {
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
        ScopedRuntimeHooks hooks;
        hooks.state.observe = true;
#endif
        ApplicationPhysics physics;
        bool caught = false;
        {
            ScopedStartupLogRefusal refusal;
            try {
                physics.startup();
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            refusal.RequireRefused();
        }
        Require(caught && JPH::Factory::sInstance == nullptr);
        RequireUnpublished(physics);
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
        Require(hooks.state.physics_acquisitions == 1 && hooks.state.temp_acquisitions == 1 &&
                hooks.state.dependency_releases == 2);
#endif
        {
            ApplicationPhysics retry;
            retry.startup();
            Require(retry.is_started());
            retry.update(1.0F / 60.0F);
        }
        Require(JPH::Factory::sInstance == nullptr);
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
        Require(hooks.state.physics_acquisitions == 2 && hooks.state.temp_acquisitions == 2 &&
                hooks.state.dependency_releases == 4);
#endif
    }
    std::_Exit(0);
}
} // namespace

TEST(JoltStartup, ApplicationRepeatedStartupShutdownKeepsAllocatorCustody) {
    ASSERT_EXIT(RepeatedApplicationLifecycle(), ::testing::ExitedWithCode(0), "");
}
#if defined(_WIN32) && !defined(JPH_DISABLE_CUSTOM_ALLOCATOR)
TEST(JoltStartup, WindowsFirstAndLaterWorkerCreationFailureJoinsAcquiredWorkers) {
    ASSERT_EXIT(WindowsPoolLaunchFailures(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, WindowsApplicationWorkerCreationFailureReleasesRuntimeAndAllowsRetry) {
    ASSERT_EXIT(WindowsApplicationCreationFailures(true), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, WindowsApplicationSemaphoreCreationFailureReleasesRuntimeAndAllowsRetry) {
    ASSERT_EXIT(WindowsApplicationCreationFailures(false), ::testing::ExitedWithCode(0), "");
}
#endif
TEST(JoltStartup, ApplicationLoggingFailureJoinsBeforeDependenciesAndAllowsRetry) {
    ASSERT_EXIT(ApplicationLogRollbackAndDestructor(), ::testing::ExitedWithCode(0), "");
}
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
TEST(JoltStartup, ApplicationColdFactoryAllocationNullAndThrowAllowRetry) {
    for (bool null_result : {false, true}) {
        ASSERT_EXIT(
            ColdApplicationRefusal(1, null_result, false), ::testing::ExitedWithCode(0), "");
    }
}
TEST(JoltStartup, ApplicationColdRegistrationNullAndThrowAllowRetry) {
    for (bool null_result : {false, true}) {
        ASSERT_EXIT(
            ColdApplicationRefusal(2, null_result, false), ::testing::ExitedWithCode(0), "");
        ASSERT_EXIT(ColdApplicationRefusal(0, null_result, true), ::testing::ExitedWithCode(0), "");
    }
}
TEST(JoltStartup, ApplicationPartialAllocatorsRefuseAndPriorMaterialSurvives) {
    ASSERT_EXIT(PriorMaterialAndPartialCallbacks(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, ApplicationCandidateNullAndThrowPreserveOwnedAndBorrowedSessions) {
    for (bool borrowed : {false, true}) {
        for (bool null_result : {false, true}) {
            ASSERT_EXIT(RetainedApplicationOwner(borrowed, null_result, false),
                        ::testing::ExitedWithCode(0),
                        "");
        }
    }
}
TEST(JoltStartup, ApplicationCandidateLoggingRollbackPreservesBothFactoryOwners) {
    for (bool borrowed : {false, true}) {
        ASSERT_EXIT(
            RetainedApplicationOwner(borrowed, false, true), ::testing::ExitedWithCode(0), "");
    }
}
#endif

TEST(JoltStartup, ApplicationLeaseStorageFailureIsIndependentOfStartedFlag) {
    ASSERT_EXIT(ApplicationLeaseStorageRefusal(), ::testing::ExitedWithCode(0), "");
}

TEST(JoltStartup, ApplicationUninitializedAndSubstitutedFactoriesRefuseWithoutMutation) {
    ASSERT_EXIT(ApplicationExternalFactoryRefusal(), ::testing::ExitedWithCode(0), "");
}
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
TEST(JoltStartup, ApplicationChangedAllocatorCallbacksRefuseAndOriginalSetAllowsRetry) {
    ASSERT_EXIT(ApplicationChangedAllocatorCallbacksRefusal(), ::testing::ExitedWithCode(0), "");
}
TEST(JoltStartup, ApplicationDefaultAllocatorsRemainCompatibleWithStaticStorage) {
    ASSERT_EXIT(ApplicationDefaultAllocatorLifetime(), ::testing::ExitedWithCode(0), "");
}
#endif
