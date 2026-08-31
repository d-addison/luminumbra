#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Luminumbra {

using Job = std::function<void()>;

struct JobCompletionState;

struct JobHandle {
    std::shared_ptr<std::atomic<int>> counter;
    std::shared_ptr<JobCompletionState> completion;
};

// Scheduling lane for dispatched jobs. High-priority work is preferred by
// workers; Normal-priority work is guaranteed forward progress via the
// starvation guard (see JobSystem::kNormalServiceInterval).
enum class JobPriority {
    High,
    Normal,
};

class JobSystem {
public:
    // Starvation guard: a worker serves at least one Normal job after this
    // many consecutive High jobs whenever Normal work is waiting, so a
    // sustained High backlog cannot stall the Normal lane indefinitely.
    static constexpr std::size_t kNormalServiceInterval = 4;

    struct RuntimeStats {
        size_t worker_count = 0;
        // Total queued jobs across both lanes. Kept as the lane-agnostic
        // depth for backward compatibility (runtime artifacts and
        // validators read this field).
        size_t queue_depth = 0;
        size_t high_priority_queue_depth = 0;
        size_t normal_priority_queue_depth = 0;
        bool accepting_jobs = false;
        bool stop_requested = false;
    };

    ~JobSystem();

    // worker_count == 0 starts one worker per hardware thread.
    void startup(std::size_t worker_count = 0);
    void shutdown();
    void dispatch(Job job, JobPriority priority = JobPriority::Normal);
    JobHandle dispatch_batch(const std::vector<Job>& jobs,
                             JobPriority priority = JobPriority::Normal);
    void wait(const JobHandle& handle);
    RuntimeStats get_runtime_stats() const;

private:
    // a pooled, allocation-free job slot. The
    // previous design wrapped every queued job in a SECOND std::function (to
    // attach the completion guard) and stored it in a std::deque-backed
    // std::queue, so each dispatch paid for (a) the wrapper std::function's
    // heap block when the {job, completion} capture exceeded the SBO -- it
    // always did, ~32 bytes > libstdc++'s 16-byte buffer -- and (b) std::deque
    // node growth. PooledJob instead stores the caller's Job and the
    // completion handle directly in a pre-sized ring (PooledQueue below), so a
    // dispatch into already-grown lanes performs ZERO heap allocations beyond
    // the caller-side std::function the public API still accepts. The
    // completion guard moved into the worker loop (see worker_loop / run_slot).
    struct PooledJob {
        Job job;
        std::shared_ptr<JobCompletionState> completion;
    };

    // a contiguous ring buffer of PooledJob slots.
    // It grows by doubling (never shrinks) so steady-state dispatch reuses
    // slots without allocating. All access is serialized by JobSystem's
    // m_queue_mutex; the ring itself carries no internal synchronization.
    class PooledQueue {
    public:
        PooledQueue();
        bool empty() const {
            return m_size == 0;
        }
        std::size_t size() const {
            return m_size;
        }
        // Moves `slot` into the ring, growing capacity if full.
        void push(PooledJob&& slot);
        // Moves the front slot out and advances the head. The vacated slot's
        // Job/shared_ptr are reset so referenced state is released promptly.
        PooledJob pop();
        // pop the slot `offset` places
        // past the head (swap-with-front then pop) — the throttle's service-
        // order perturbation primitive. offset must be < size.
        PooledJob pop_at(std::size_t offset);

    private:
        void grow();

        std::vector<PooledJob> m_slots;
        std::size_t m_head = 0;
        std::size_t m_tail = 0;
        std::size_t m_size = 0;
    };

    void worker_loop();
    // Requires m_queue_mutex to be held.
    PooledQueue& queue_for(JobPriority priority);
    // the fast/slow-job determinism
    // axis. When LUMINUMBRA_JOB_THROTTLE=<seed> is set at startup, workers pop
    // a SplitMix64-selected slot from a small window at the queue head instead
    // of the front — an adversarial, wall-clock-free perturbation of job
    // SERVICE ORDER (:359: a naive order-preserving sleep proves
    // nothing). Off by default: one branch on a bool, hash-neutral-off by
    // construction. The determinism matrix asserts sim hashes are INVARIANT
    // under it. Requires m_queue_mutex to be held.
    PooledJob throttled_pop(PooledQueue& queue);
    // run a popped slot and complete its batch on
    // every exit path. Static (no JobSystem state) but a member so it can touch
    // the private PooledJob type.
    static void run_slot(PooledJob& slot);

    std::vector<std::thread> m_workers;
    PooledQueue m_high_queue;
    PooledQueue m_normal_queue;
    // Consecutive High jobs served while Normal work waited; guarded by
    // m_queue_mutex.
    std::size_t m_consecutive_high_served = 0;
    //  throttle state; counter guarded by m_queue_mutex.
    bool m_throttle_enabled = false;
    std::uint64_t m_throttle_seed = 0;
    std::uint64_t m_throttle_pop_counter = 0;
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop_threads = false;
    bool m_accepting_jobs = false;
};

} // namespace Luminumbra
