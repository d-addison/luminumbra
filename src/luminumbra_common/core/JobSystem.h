#pragma once

#include <cstddef>
#include <functional>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

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
    JobHandle dispatch_batch(const std::vector<Job>& jobs, JobPriority priority = JobPriority::Normal);
    void wait(const JobHandle& handle);
    RuntimeStats get_runtime_stats() const;

private:
    void worker_loop();
    // Requires m_queue_mutex to be held.
    std::queue<Job>& queue_for(JobPriority priority);

    std::vector<std::thread> m_workers;
    std::queue<Job> m_high_queue;
    std::queue<Job> m_normal_queue;
    // Consecutive High jobs served while Normal work waited; guarded by
    // m_queue_mutex.
    std::size_t m_consecutive_high_served = 0;
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop_threads = false;
    bool m_accepting_jobs = false;

};

} // namespace Luminumbra
