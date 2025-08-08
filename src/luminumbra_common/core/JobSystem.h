#pragma once

#include <functional>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array> // <-- ADDED

namespace Luminumbra {

using Job = std::function<void()>;

struct JobHandle {
    std::atomic<int>* counter = nullptr;
};

class JobSystem {
public:
    void startup();
    void shutdown();
    void dispatch(Job job);
    JobHandle dispatch_batch(const std::vector<Job>& jobs);
    void wait(const JobHandle& handle);

private:
    void worker_loop();

    std::vector<std::thread> m_workers;
    std::queue<Job> m_job_queue;
    std::mutex m_queue_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop_threads = false;

    // For the batch system
    // CHANGED from std::vector to std::array to solve copy/move issue with std::atomic
    std::array<std::atomic<int>, 256> m_counters;
    std::atomic<int> m_next_counter_index = 0;
};

} // namespace Luminumbra