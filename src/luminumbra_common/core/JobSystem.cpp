#include "JobSystem.h"
#include <iostream>
#include "../../../include/luminumbra/core/Types.h"
#include "core/Log.h"

namespace Luminumbra {

void JobSystem::startup() {
    const u32 num_threads = std::thread::hardware_concurrency();
    m_workers.reserve(num_threads);
    for (u32 i = 0; i < num_threads; ++i) {
        m_workers.emplace_back(&JobSystem::worker_loop, this);
    }
    LUMINUMBRA_CORE_INFO("JobSystem started with " + std::to_string(num_threads) + " threads.");
}

void JobSystem::shutdown() {
    m_stop_threads.store(true);
    m_condition.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void JobSystem::dispatch(Job job) {
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        m_job_queue.push(std::move(job));
    }
    m_condition.notify_one();
}

JobHandle JobSystem::dispatch_batch(const std::vector<Job>& jobs) {
    if (jobs.empty()) {
        return JobHandle{};
    }

    auto counter = std::make_shared<std::atomic<int>>(static_cast<int>(jobs.size()));
    JobHandle handle{counter};

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        for (const auto& job : jobs) {
            m_job_queue.push([job, counter]() {
                job();
                counter->fetch_sub(1);
            });
        }
    }

    m_condition.notify_all();
    return handle;
}

void JobSystem::wait(const JobHandle& handle) {
    if (handle.counter) {
        while (handle.counter->load() > 0) {
            std::this_thread::yield();
        }
    }
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
            job();
        }
    }
}

} // namespace Luminumbra
