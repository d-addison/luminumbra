#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace Luminumbra {

// Blocking storage work has a separate, fixed-size lane. Lifecycle calls belong
// to the owning thread; shutdown drains accepted work before joining workers.
class IoExecutor {
public:
    static constexpr std::size_t kWorkerCount = 2;
    // At most 64 queued payloads plus two executing payloads are retained.
    static constexpr std::size_t kQueueCapacity = 64;

    ~IoExecutor();
    void startup(std::uint64_t throttle_seed = 0);
    void shutdown();
    // Waits for queue space, refusing only invalid work or a closed executor.
    // The caller keeps operation's storage alive until its task finishes.
    bool dispatch(void (*task)(void*), std::shared_ptr<void> payload, std::string_view operation);
    // Nonblocking admission, also refusing when the fixed queue is full.
    bool
    try_dispatch(void (*task)(void*), std::shared_ptr<void> payload, std::string_view operation);
    bool is_current_worker() const noexcept;

private:
    struct Task {
        void (*execute)(void*) = nullptr;
        std::shared_ptr<void> payload;
        std::string_view operation;
    };

    bool admit(Task task, bool wait_for_space);
    void worker_loop();

    std::array<std::thread, kWorkerCount> m_workers;
    std::array<Task, kQueueCapacity> m_queue;
    std::size_t m_head = 0;
    std::size_t m_size = 0;
    std::uint64_t m_throttle_seed = 0;
    std::uint64_t m_pop_counter = 0;
    std::mutex m_mutex;
    std::condition_variable m_work_available;
    std::condition_variable m_space_available;
    bool m_accepting = false;
};

} // namespace Luminumbra
