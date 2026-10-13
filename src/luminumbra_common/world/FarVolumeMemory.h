#pragma once

// Byte accounting for far-volume generation, meshing and facade compilation.
//
// Model: a FarVolumeMemoryPool is a thread-safe byte budget. A FarVolumeMemoryLease
// is a move-only holder of bytes reserved from one pool. Every allocation that an
// operation makes on its own behalf is admitted (reserved) BEFORE it is requested
// from the system; a refused admission allocates nothing and unwinds the operation
// with a typed AllocationFailure. There is no fallback allocation path.
//
// Charged: tile bricks, mesh vertices/indices, mesher sample/gradient/edge caches,
// brick ordering scratch, the facade height cache and checksum scratch. Vector
// outputs are charged at their exact capacity, and growth charges the new buffer
// while the old one is still held (peak = old + new).
// Not charged: memory owned by caller callbacks (including copies of the
// std::function targets), the operation's own std::function adapters, exception
// objects, and allocator bookkeeping below the requested size.
// The unbound (null pool) path is the pre-existing unaccounted behaviour.
//
// Threading: a pool is thread-safe and may be shared by concurrent operations. A
// lease, a scope and an output object are single-owner: each concurrent operation
// needs its own output and its own `retained` lease, and a retained lease must stay
// paired with the one output it describes (never reuse another output's lease). A
// caller-supplied `upstream` must itself be thread-safe if operations share it.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Luminumbra::World {

// Thrown only by FarVolumeChargedResource when admission is refused. It is a
// std::bad_alloc so existing allocation-failure handling maps it unchanged.
class FarVolumeMemoryRefused : public std::bad_alloc {
public:
    const char* what() const noexcept override {
        return "far-volume memory budget refused";
    }
};

class FarVolumeMemoryPool {
public:
    explicit FarVolumeMemoryPool(std::uint64_t limit_bytes) noexcept
        : limit_(limit_bytes) {}
    FarVolumeMemoryPool(const FarVolumeMemoryPool&) = delete;
    FarVolumeMemoryPool& operator=(const FarVolumeMemoryPool&) = delete;

    // Atomically admits `bytes`. Never throws, never overflows. A zero request
    // always succeeds and is not counted as an attempt.
    bool TryReserve(std::uint64_t bytes) noexcept {
        if (bytes == 0)
            return true;
        const auto attempt = attempts_.fetch_add(1, std::memory_order_relaxed);
        if (attempt >= fail_after_.load(std::memory_order_relaxed)) {
            refusals_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto used = used_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > limit_ || used > limit_ - bytes) {
                refusals_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (used_.compare_exchange_weak(
                    used, used + bytes, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
        const auto now = used + bytes;
        auto peak = peak_.load(std::memory_order_relaxed);
        while (peak < now && !peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
        return true;
    }
    // Returns bytes previously admitted. Releasing more than is held clamps to
    // zero rather than wrapping.
    void Release(std::uint64_t bytes) noexcept {
        auto used = used_.load(std::memory_order_relaxed);
        while (!used_.compare_exchange_weak(used,
                                            used > bytes ? used - bytes : 0,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {}
    }

    // Diagnostic fault injection: refuse the attempt numbered `count` (zero-based,
    // counted from construction) and every later one. Used to prove that every
    // admission point unwinds without leaking or replacing prior output.
    void RefuseAttemptsFrom(std::uint64_t count) noexcept {
        fail_after_.store(count, std::memory_order_relaxed);
    }

    std::uint64_t limit() const noexcept {
        return limit_;
    }
    std::uint64_t used() const noexcept {
        return used_.load(std::memory_order_relaxed);
    }
    std::uint64_t peak() const noexcept {
        return peak_.load(std::memory_order_relaxed);
    }
    std::uint64_t attempts() const noexcept {
        return attempts_.load(std::memory_order_relaxed);
    }
    std::uint64_t refusals() const noexcept {
        return refusals_.load(std::memory_order_relaxed);
    }

private:
    const std::uint64_t limit_;
    std::atomic<std::uint64_t> used_{0}, peak_{0}, attempts_{0}, refusals_{0};
    std::atomic<std::uint64_t> fail_after_{std::numeric_limits<std::uint64_t>::max()};
};

class FarVolumeMemoryLease {
public:
    FarVolumeMemoryLease() noexcept = default;
    explicit FarVolumeMemoryLease(FarVolumeMemoryPool& pool) noexcept
        : pool_(&pool) {}
    FarVolumeMemoryLease(const FarVolumeMemoryLease&) = delete;
    FarVolumeMemoryLease& operator=(const FarVolumeMemoryLease&) = delete;
    FarVolumeMemoryLease(FarVolumeMemoryLease&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr))
        , bytes_(std::exchange(other.bytes_, 0)) {}
    // Adopts `other` and then returns this lease's previous bytes to its pool, so
    // replacing a retained output never requires new admission.
    FarVolumeMemoryLease& operator=(FarVolumeMemoryLease&& other) noexcept {
        if (this != &other) {
            Reset();
            pool_ = std::exchange(other.pool_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }
    ~FarVolumeMemoryLease() {
        Reset();
    }

    // Adds `bytes` to this lease. Fails (changing nothing) when unbound, on
    // overflow, or when the pool refuses.
    bool TryCharge(std::uint64_t bytes) noexcept {
        if (pool_ == nullptr || bytes > std::numeric_limits<std::uint64_t>::max() - bytes_)
            return false;
        if (!pool_->TryReserve(bytes))
            return false;
        bytes_ += bytes;
        return true;
    }
    void Uncharge(std::uint64_t bytes) noexcept {
        if (pool_ == nullptr)
            return;
        bytes = bytes < bytes_ ? bytes : bytes_;
        pool_->Release(bytes);
        bytes_ -= bytes;
    }
    // Merges another lease from the same pool into this one without touching the pool.
    bool Absorb(FarVolumeMemoryLease&& other) noexcept {
        if (other.bytes_ == 0) {
            return true;
        }
        if (pool_ == nullptr || other.pool_ != pool_ ||
            other.bytes_ > std::numeric_limits<std::uint64_t>::max() - bytes_)
            return false;
        bytes_ += std::exchange(other.bytes_, 0);
        return true;
    }
    void Reset() noexcept {
        if (pool_ != nullptr && bytes_ != 0)
            pool_->Release(bytes_);
        bytes_ = 0;
    }

    std::uint64_t bytes() const noexcept {
        return bytes_;
    }
    const FarVolumeMemoryPool* pool() const noexcept {
        return pool_;
    }

private:
    FarVolumeMemoryPool* pool_ = nullptr;
    std::uint64_t bytes_ = 0;
};

// Optional binding passed to the *Charged entry points. A null pool selects the
// unaccounted legacy path. With a pool, `retained` receives the successful
// output's bytes: it is replaced only on success (the previous output's bytes stay
// charged while the replacement is built) and is left untouched on any refusal.
// `retained` may be null only for operations that return no retained output.
// `upstream` supplies the real allocator for charged scratch containers; null
// means std::pmr::new_delete_resource(). A diagnostic upstream lets tests observe
// that nothing is requested from the system after a refusal.
struct FarVolumeMemory {
    FarVolumeMemoryPool* pool = nullptr;
    FarVolumeMemoryLease* retained = nullptr;
    std::pmr::memory_resource* upstream = nullptr;
};

// A pmr resource that reserves from a lease before delegating, and returns the
// reservation on deallocation. Refusal throws FarVolumeMemoryRefused and calls
// upstream not at all.
class FarVolumeChargedResource final : public std::pmr::memory_resource {
public:
    FarVolumeChargedResource(FarVolumeMemoryLease& lease,
                             std::pmr::memory_resource* upstream = nullptr) noexcept
        : lease_(&lease)
        , upstream_(upstream != nullptr ? upstream : std::pmr::new_delete_resource()) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (!lease_->TryCharge(bytes))
            throw FarVolumeMemoryRefused();
        try {
            return upstream_->allocate(bytes, alignment);
        } catch (...) {
            lease_->Uncharge(bytes);
            throw;
        }
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes, alignment);
        lease_->Uncharge(bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    FarVolumeMemoryLease* lease_;
    std::pmr::memory_resource* upstream_;
};

// Per-operation context. Declare it BEFORE any container that uses resource() so
// the containers are destroyed (returning their bytes) first.
class FarVolumeMemoryScope {
public:
    // `needs_retained` is true for operations that publish a retained output.
    FarVolumeMemoryScope(const FarVolumeMemory& memory, bool needs_retained) noexcept
        : memory_(memory)
        , transient_(memory.pool != nullptr ? FarVolumeMemoryLease(*memory.pool)
                                            : FarVolumeMemoryLease())
        , output_(memory.pool != nullptr ? FarVolumeMemoryLease(*memory.pool)
                                         : FarVolumeMemoryLease())
        , resource_(transient_, memory.upstream)
        , needs_retained_(needs_retained) {}
    FarVolumeMemoryScope(const FarVolumeMemoryScope&) = delete;
    FarVolumeMemoryScope& operator=(const FarVolumeMemoryScope&) = delete;

    bool bounded() const noexcept {
        return memory_.pool != nullptr;
    }
    // False for a pool without the required retained lease, or one from another pool.
    bool valid() const noexcept {
        if (memory_.pool == nullptr)
            return true;
        if (!needs_retained_)
            return true;
        return memory_.retained != nullptr &&
               (memory_.retained->pool() == nullptr || memory_.retained->pool() == memory_.pool);
    }
    // Scratch allocator for pmr containers: charged when bounded, otherwise the
    // upstream (default new/delete).
    std::pmr::memory_resource* resource() noexcept {
        if (memory_.pool != nullptr)
            return &resource_;
        return memory_.upstream != nullptr ? memory_.upstream : std::pmr::new_delete_resource();
    }
    // Lease for the output being built; null when unbounded.
    FarVolumeMemoryLease* output() noexcept {
        return memory_.pool != nullptr ? &output_ : nullptr;
    }
    // Publishes the built output's bytes. Call only after every step that can
    // refuse has succeeded and AFTER the output has been assigned, so the previous
    // output's buffers are freed before its charge is released; it cannot fail or throw.
    void Commit() noexcept {
        if (memory_.pool != nullptr && memory_.retained != nullptr)
            *memory_.retained = std::move(output_);
    }

private:
    FarVolumeMemory memory_;
    FarVolumeMemoryLease transient_, output_;
    FarVolumeChargedResource resource_;
    bool needs_retained_;
};

// Grows `vector` to hold at least `needed` elements, geometrically but never beyond
// max(`cap`, `needed`), charging the new buffer to `lease` BEFORE allocating it.
// Precondition: the vector's whole current capacity is charged to `lease` (it was
// only ever grown by this function); otherwise the call refuses. A null lease is the
// unaccounted path and does nothing (the caller's push_back grows normally).
// The replacement is built in a separate buffer, so a refusal or an exception leaves
// the vector and the lease exactly as they were. Peak charge is old + new. If an
// implementation rounds reserve() up, the surplus is charged immediately after the
// allocation and rolled back (buffer freed before the charge is released) on refusal.
// That is the one case where a system allocation precedes a refusal, so the guarantee
// "no allocation on refusal" is narrowed to implementations whose reserve(n) yields
// capacity n: libstdc++, libc++ and the MSVC STL do, and a test pins it.
// Returns false on refusal.
template<class T>
bool FarVolumeReserveCharged(std::vector<T>& vector,
                             std::size_t needed,
                             std::size_t cap,
                             FarVolumeMemoryLease* lease) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (lease == nullptr || needed <= vector.capacity())
        return true;
    const std::uint64_t old_bytes = static_cast<std::uint64_t>(vector.capacity()) * sizeof(T);
    if (old_bytes > lease->bytes())
        return false;
    std::size_t target = vector.capacity() < 16 ? 16 : vector.capacity() * 2;
    if (target > cap)
        target = cap;
    if (target < needed)
        target = needed;
    if (target > vector.max_size() ||
        target > std::numeric_limits<std::uint64_t>::max() / sizeof(T))
        return false;
    const std::uint64_t new_bytes = static_cast<std::uint64_t>(target) * sizeof(T);
    if (!lease->TryCharge(new_bytes))
        return false;
    std::vector<T> replacement;
    try {
        replacement.reserve(target);
    } catch (...) {
        lease->Uncharge(new_bytes);
        throw;
    }
    const std::uint64_t actual = static_cast<std::uint64_t>(replacement.capacity()) * sizeof(T);
    if (actual > new_bytes && !lease->TryCharge(actual - new_bytes)) {
        std::vector<T>().swap(replacement); // free before releasing the charge
        lease->Uncharge(new_bytes);
        return false;
    }
    replacement.insert(replacement.end(), vector.begin(), vector.end());
    vector.swap(replacement);
    std::vector<T>().swap(replacement); // free the old buffer before releasing its charge
    lease->Uncharge(old_bytes);
    return true;
}

} // namespace Luminumbra::World
