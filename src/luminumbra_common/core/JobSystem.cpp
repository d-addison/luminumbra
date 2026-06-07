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
    // No need to resize std::array or initialize atomics, they default to 0.
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

    int counter_index = m_next_counter_index.fetch_add(1) % m_counters.size();
    std::atomic<int>* counter_ptr = &m_counters[counter_index];
    counter_ptr->store((int)jobs.size());

    JobHandle handle{counter_ptr}; // The handle now stores this stable pointer.

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        for (const auto& job : jobs) {
            // Capture the POINTER by value.
            m_job_queue.push([job, counter_ptr]() {
                job();
                counter_ptr->fetch_sub(1); // Use the pointer, which is valid.
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