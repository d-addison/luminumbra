#pragma once

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

class JobSystem {
public:
    ~JobSystem();

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
    bool m_accepting_jobs = false;

};

} // namespace Luminumbra
