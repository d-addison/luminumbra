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
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
        }
        completion->condition.notify_all();
    }
}

} // namespace

JobSystem::~JobSystem() {
    shutdown();
}

void JobSystem::startup() {
    u32 num_threads = std::thread::hardware_concurrency();
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
    }

    m_workers.reserve(num_threads);
    for (u32 i = 0; i < num_threads; ++i) {
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

void JobSystem::dispatch(Job job) {
    if (!job) {
        return;
    }

    bool accepted = false;
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            m_job_queue.push(std::move(job));
            accepted = true;
        }
    }

    if (!accepted) {
        LUMINUMBRA_CORE_WARN("JobSystem rejected job dispatch while shutting down.");
        return;
    }

    m_condition.notify_one();
}

JobHandle JobSystem::dispatch_batch(const std::vector<Job>& jobs) {
    if (jobs.empty()) {
        return JobHandle{};
    }

    auto completion = std::make_shared<JobCompletionState>(static_cast<int>(jobs.size()));
    JobHandle handle{std::shared_ptr<std::atomic<int>>(completion, &completion->counter), completion};
    bool accepted = false;

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            for (const auto& job : jobs) {
                m_job_queue.push([job, completion]() {
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
        completion->counter.store(0, std::memory_order_release);
        completion->condition.notify_all();
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
    stats.queue_depth = m_job_queue.size();
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
                return !m_job_queue.empty() || m_stop_threads.load();
            });

            if (m_stop_threads.load() && m_job_queue.empty()) {
                return;
            }

            job = std::move(m_job_queue.front());
            m_job_queue.pop();
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
