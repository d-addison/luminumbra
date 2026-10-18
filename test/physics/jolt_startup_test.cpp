#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
};
AllocationLedger ledger;

void Require(bool value, int code = 91) {
    if (!value) {
        std::_Exit(code);
    }
}

void Record(void* block) {
    if (block == nullptr || !ledger.capture.load()) {
        return;
    }
    while (ledger.lock.test_and_set(std::memory_order_acquire)) {}
    for (void*& slot : ledger.blocks) {
        if (slot == nullptr) {
            slot = block;
            ledger.lock.clear(std::memory_order_release);
            return;
        }
    }
    std::_Exit(92);
}

void Forget(void* block) {
    while (ledger.lock.test_and_set(std::memory_order_acquire)) {}
    for (void*& slot : ledger.blocks) {
        if (slot == block) {
            slot = nullptr;
            break;
        }
    }
    ledger.lock.clear(std::memory_order_release);
}

std::size_t Outstanding() {
    std::size_t count = 0;
    for (void* block : ledger.blocks) {
        count += block != nullptr ? 1U : 0U;
    }
    return count;
}

bool Refuse() {
    if (!ledger.capture.load() ||
        (ledger.workers_only.load() && std::this_thread::get_id() == ledger.creator)) {
        return false;
    }
    const std::size_t ordinal = ledger.attempts.fetch_add(1) + 1;
    const std::size_t selected = ledger.fail_at.load();
    if (selected == 0 || ordinal < selected || (!ledger.continue_refusing && ordinal != selected)) {
        return false;
    }
    ledger.failures.fetch_add(1);
    return true;
}

void Arm(std::size_t ordinal, bool null_result, bool continued, bool workers = false) {
    ledger.creator = std::this_thread::get_id();
    ledger.attempts = 0;
    ledger.failures = 0;
    ledger.fail_at = ordinal;
    ledger.return_null = null_result;
    ledger.continue_refusing = continued;
    ledger.workers_only = workers;
    ledger.capture = true;
}

void Disarm() {
    ledger.capture = false;
    ledger.fail_at = 0;
    ledger.workers_only = false;
}

void* AllocateBytes(std::size_t size, std::size_t alignment, bool nullable) {
    if (Refuse()) {
        if (nullable && ledger.return_null) {
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
        if (ledger.return_null) {
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
    const std::size_t measured = ledger.attempts.load();
    Disarm();
    Require(measured > 0 && measured < ledger.blocks.size());
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
                const bool reached = ledger.failures.load() != 0;
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
        Require(caught && ledger.failures.load() == 1);
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
                measured = ledger.attempts.load();
                Require(measured > 0 && measured < ledger.blocks.size());
            } else {
                Require(caught && ledger.failures.load() != 0);
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
        return std::count(ledger.blocks.begin(), ledger.blocks.end(), block) == 1;
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
    const std::size_t teardown_allocations = ledger.attempts.load();
    Disarm();
    Require(teardown_allocations == 0 && Outstanding() == 0);
    JPH::UnregisterTypes();
    JPH::Factory::sInstance = nullptr;
    std::_Exit(0);
}

#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
void FactoryGrowth() {
    InitializeAllocators();
    JPH::RTTI sentinel("Sentinel", 1, nullptr, nullptr);
    JPH::RTTI extra("Extra", 1, nullptr, nullptr);
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
                measured = ledger.attempts.load();
                Require(measured > 0 && measured < ledger.blocks.size());
            } else {
                Require(caught && ledger.failures.load() != 0);
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
struct NativeLedger {
    std::atomic<unsigned> launch_attempts{0};
    std::atomic<unsigned> launch_failure{0};
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> joined{0};
    unsigned semaphore_attempts = 0;
    unsigned semaphore_failure = 0;
    unsigned semaphore_acquired = 0;
    unsigned semaphore_closed = 0;
    std::array<sem_t*, 32> semaphores{};
    bool active = false;
};
NativeLedger native;
using CreateThread = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
using JoinThread = int (*)(pthread_t, void**);
using InitSemaphore = int (*)(sem_t*, int, unsigned);
using DestroySemaphore = int (*)(sem_t*);
CreateThread real_create = nullptr;
JoinThread real_join = nullptr;
InitSemaphore real_sem_init = nullptr;
DestroySemaphore real_sem_destroy = nullptr;

void ResolveNative() {
    real_create = reinterpret_cast<CreateThread>(dlsym(RTLD_NEXT, "pthread_create"));
    real_join = reinterpret_cast<JoinThread>(dlsym(RTLD_NEXT, "pthread_join"));
    real_sem_init = reinterpret_cast<InitSemaphore>(dlsym(RTLD_NEXT, "sem_init"));
    real_sem_destroy = reinterpret_cast<DestroySemaphore>(dlsym(RTLD_NEXT, "sem_destroy"));
    Require(real_create && real_join && real_sem_init && real_sem_destroy);
}

void ResetNative() {
    native.launch_attempts = 0;
    native.launch_failure = 0;
    native.created = 0;
    native.joined = 0;
    native.semaphore_attempts = 0;
    native.semaphore_failure = 0;
    native.semaphore_acquired = 0;
    native.semaphore_closed = 0;
    for (auto& semaphore : native.semaphores) {
        semaphore = nullptr;
    }
    native.active = true;
}

void BeginNativeAccounting() {
    ResolveNative();
    ResetNative();
}
void EndNativeAccounting(unsigned expected) {
    Require(native.created.load() == expected && native.joined.load() == expected);
    Require(native.semaphore_acquired == native.semaphore_closed);
    native.active = false;
}

void ThreadLaunchFailures() {
    InitializeAllocators();
    ResolveNative();
    for (unsigned failure : {1U, 2U}) {
        ResetNative();
        native.launch_failure = failure;
        Arm(0, false, false);
        bool caught = false;
        try {
            JPH::JobSystemThreadPool pool(2048, 8, 3);
        } catch (const std::system_error& error) {
            caught = error.code().value() == EAGAIN;
        }
        Disarm();
        Require(caught && native.launch_attempts == failure);
        Require(native.created == failure - 1 && native.joined == native.created);
        Require(native.semaphore_acquired == native.semaphore_closed && Outstanding() == 0);
        native.active = false;
    }
    // Failed replacement stops the old pool and owns no replacement worker.
    ResetNative();
    {
        JPH::JobSystemThreadPool pool(2048, 8, 1);
        native.launch_failure = native.launch_attempts.load() + 1;
        bool caught = false;
        try {
            pool.SetNumThreads(3);
        } catch (const std::system_error& error) {
            caught = error.code().value() == EAGAIN;
        }
        Require(caught && pool.GetMaxConcurrency() == 1);
        Require(native.joined == native.created);
    }
    Require(native.semaphore_acquired == native.semaphore_closed);
    native.active = false;
    std::_Exit(0);
}

void SemaphoreFailures() {
    InitializeAllocators();
    ResolveNative();
    // One pool semaphore and eight real BarrierImpl semaphore members.
    ResetNative();
    { JPH::JobSystemThreadPool pool(2048, 8, 0); }
    const unsigned measured = native.semaphore_attempts;
    Require(measured > 1 && native.semaphore_acquired == native.semaphore_closed);
    native.active = false;
    for (unsigned failure = 1; failure <= measured; ++failure) {
        ResetNative();
        native.semaphore_failure = failure;
        Arm(0, false, false);
        bool caught = false;
        try {
            JPH::JobSystemThreadPool pool(2048, 8, 0);
        } catch (const std::system_error& error) {
            caught = error.code().value() == ENOSPC;
        }
        Disarm();
        Require(caught && native.semaphore_attempts == failure);
        Require(native.semaphore_acquired == failure - 1);
        Require(native.semaphore_closed == native.semaphore_acquired && Outstanding() == 0);
        native.active = false;
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
    if (real_create == nullptr) {
        ResolveNative();
    }
    if (native.active) {
        const unsigned attempt = native.launch_attempts.fetch_add(1) + 1;
        if (attempt == native.launch_failure.load()) {
            return EAGAIN;
        }
    }
    const int result = real_create(thread, attributes, entry, argument);
    if (native.active && result == 0) {
        native.created.fetch_add(1);
    }
    return result;
}
extern "C" int pthread_join(pthread_t thread, void** result) {
    if (real_join == nullptr) {
        ResolveNative();
    }
    const int status = real_join(thread, result);
    if (native.active && status == 0) {
        native.joined.fetch_add(1);
    }
    return status;
}
extern "C" int sem_init(sem_t* semaphore, int shared, unsigned value) noexcept {
    if (real_sem_init == nullptr) {
        ResolveNative();
    }
    if (native.active && ++native.semaphore_attempts == native.semaphore_failure) {
        errno = ENOSPC;
        return -1;
    }
    const int result = real_sem_init(semaphore, shared, value);
    if (native.active && result == 0) {
        Require(native.semaphore_acquired < native.semaphores.size());
        native.semaphores[native.semaphore_acquired++] = semaphore;
    }
    return result;
}
extern "C" int sem_destroy(sem_t* semaphore) noexcept {
    if (real_sem_destroy == nullptr) {
        ResolveNative();
    }
    if (native.active) {
        bool found = false;
        for (auto& owned : native.semaphores) {
            if (owned == semaphore) {
                Require(!found);
                owned = nullptr;
                found = true;
            }
        }
        Require(found);
    }
    const int result = real_sem_destroy(semaphore);
    if (native.active && result == 0) {
        ++native.semaphore_closed;
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
struct alignas(64) AlignedAllocation {
    JPH_OVERRIDE_NEW_DELETE
    unsigned value = 7;
};

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
        Require(caught && ledger.failures.load() != 0 && Outstanding() == 0);
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
    const auto attempts = ledger.attempts.load();
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
            Require(ledger.attempts.load() == 1);
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
    const std::size_t measured = ledger.attempts.load();
    Disarm();
    Require(measured > 0 && measured < ledger.blocks.size() && Outstanding() == 0);
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
                Require(caught && ledger.failures.load() != 0 && Outstanding() == 0);
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
