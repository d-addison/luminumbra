#include "JobSystem.h"
#include <exception>
#include <string>
#include "../../../include/luminumbra/core/Types.h"
#include "core/Log.h"

namespace Luminumbra {

struct JobCompletionState {
    explicit JobCompletionState(int job_count)
        : counter(job_count) {}

    std::atomic<int> counter;
    std::mutex mutex;
    std::condition_variable condition;
};

namespace {

void complete_job(const std::shared_ptr<JobCompletionState>& completion) {
    if (!completion) {
        return;
    }

    if (completion->counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // T-I4-DR-server-streaming-race: notify with completion->mutex HELD (the
        // canonical condition_variable idiom) rather than after a momentary empty
        // lock. The waiter (JobSystem::wait) evaluates `counter <= 0` under this
        // mutex before blocking; the counter is an atomic decremented OUTSIDE the
        // mutex, so holding the lock across the notify guarantees the waiter is
        // either pre-predicate (observes 0, never blocks) or already enqueued on
        // the CV (receives the notify) -- the wakeup cannot slip into the gap
        // between the waiter releasing the mutex inside wait() and finishing its
        // CV enqueue. Hardening (not the root cause of this task's crash, which
        // was a FastNoise SIMD over-read), kept because it is the correct idiom.
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->condition.notify_all();
    }
}

} // namespace

JobSystem::~JobSystem() {
    shutdown();
}

void JobSystem::startup(std::size_t worker_count) {
    std::size_t num_threads = worker_count;
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
    }
    if (num_threads == 0) {
        num_threads = 1;
    }

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs || !m_workers.empty()) {
            LUMINUMBRA_CORE_WARN("JobSystem startup requested while already running.");
            return;
        }

        m_stop_threads.store(false, std::memory_order_release);
        m_accepting_jobs = true;
        m_consecutive_high_served = 0;
    }

    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        m_workers.emplace_back(&JobSystem::worker_loop, this);
    }
    LUMINUMBRA_CORE_INFO("JobSystem started with " + std::to_string(num_threads) + " threads.");
}

void JobSystem::shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (!m_accepting_jobs && m_workers.empty()) {
            return;
        }

        m_accepting_jobs = false;
        m_stop_threads.store(true, std::memory_order_release);
    }

    m_condition.notify_all();

    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_workers.clear();
    }
}

std::queue<Job>& JobSystem::queue_for(JobPriority priority) {
    return priority == JobPriority::High ? m_high_queue : m_normal_queue;
}

void JobSystem::dispatch(Job job, JobPriority priority) {
    if (!job) {
        return;
    }

    bool accepted = false;
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            queue_for(priority).push(std::move(job));
            accepted = true;
        }
    }

    if (!accepted) {
        LUMINUMBRA_CORE_WARN("JobSystem rejected job dispatch while shutting down.");
        return;
    }

    m_condition.notify_one();
}

JobHandle JobSystem::dispatch_batch(const std::vector<Job>& jobs, JobPriority priority) {
    if (jobs.empty()) {
        return JobHandle{};
    }

    auto completion = std::make_shared<JobCompletionState>(static_cast<int>(jobs.size()));
    JobHandle handle{std::shared_ptr<std::atomic<int>>(completion, &completion->counter), completion};
    bool accepted = false;

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            std::queue<Job>& queue = queue_for(priority);
            for (const auto& job : jobs) {
                queue.push([job, completion]() {
                    struct CompletionGuard {
                        std::shared_ptr<JobCompletionState> completion;

                        ~CompletionGuard() {
                            complete_job(completion);
                        }
                    } guard{completion};

                    if (job) {
                        job();
                    }
                });
            }
            accepted = true;
        }
    }

    if (!accepted) {
        // T-I4-DR-server-streaming-race: same lost-wakeup discipline as
        // complete_job -- drop the counter to 0 and notify with completion->mutex
        // HELD so a waiter that called wait() concurrently with this rejection
        // cannot miss the wakeup.
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->counter.store(0, std::memory_order_release);
            completion->condition.notify_all();
        }
        LUMINUMBRA_CORE_WARN("JobSystem rejected batch dispatch while shutting down.");
        return handle;
    }

    m_condition.notify_all();
    return handle;
}

void JobSystem::wait(const JobHandle& handle) {
    if (handle.completion) {
        std::unique_lock<std::mutex> lock(handle.completion->mutex);
        handle.completion->condition.wait(lock, [&handle] {
            return handle.completion->counter.load(std::memory_order_acquire) <= 0;
        });
    } else if (handle.counter) {
        while (handle.counter->load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
    }
}

JobSystem::RuntimeStats JobSystem::get_runtime_stats() const {
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    RuntimeStats stats;
    stats.worker_count = m_workers.size();
    stats.high_priority_queue_depth = m_high_queue.size();
    stats.normal_priority_queue_depth = m_normal_queue.size();
    stats.queue_depth = stats.high_priority_queue_depth + stats.normal_priority_queue_depth;
    stats.accepting_jobs = m_accepting_jobs;
    stats.stop_requested = m_stop_threads.load(std::memory_order_acquire);
    return stats;
}

void JobSystem::worker_loop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_condition.wait(lock, [this] {
                return !m_high_queue.empty() || !m_normal_queue.empty() || m_stop_threads.load();
            });

            if (m_stop_threads.load() && m_high_queue.empty() && m_normal_queue.empty()) {
                return;
            }

            // Prefer the High lane, but serve a Normal job after
            // kNormalServiceInterval consecutive High jobs so a sustained
            // High backlog cannot starve Normal work. The counter also
            // accumulates while only High work exists, so Normal work that
            // arrives behind a long High burst is served promptly.
            const bool high_available = !m_high_queue.empty();
            const bool normal_available = !m_normal_queue.empty();
            const bool serve_normal = normal_available &&
                (!high_available || m_consecutive_high_served >= kNormalServiceInterval);

            if (serve_normal) {
                job = std::move(m_normal_queue.front());
                m_normal_queue.pop();
                m_consecutive_high_served = 0;
            } else {
                job = std::move(m_high_queue.front());
                m_high_queue.pop();
                ++m_consecutive_high_served;
            }
        }

        if (job) {
            try {
                job();
            } catch (const std::exception& exception) {
                LUMINUMBRA_CORE_ERROR("JobSystem worker caught job exception: " + std::string(exception.what()));
            } catch (...) {
                LUMINUMBRA_CORE_ERROR("JobSystem worker caught unknown job exception.");
            }
        }
    }
}

} // namespace Luminumbra
