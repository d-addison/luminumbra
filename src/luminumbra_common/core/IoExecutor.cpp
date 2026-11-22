#include "IoExecutor.h"

#include "core/Log.h"
#include "core/Profiler.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace Luminumbra {
namespace {
IoExecutor*& CurrentIoExecutor() {
    static thread_local IoExecutor* executor = nullptr;
    return executor;
}
} // namespace

IoExecutor::~IoExecutor() {
    shutdown();
}

void IoExecutor::startup(std::uint64_t throttle_seed) {
    {
        const std::lock_guard lock(m_mutex);
        if (m_accepting) {
            return;
        }
        m_throttle_seed = throttle_seed;
        m_pop_counter = 0;
        m_accepting = true;
    }
    try {
        for (auto& worker : m_workers) {
            worker = std::thread(&IoExecutor::worker_loop, this);
        }
    } catch (...) {
        shutdown();
        throw;
    }
}

void IoExecutor::shutdown() {
    {
        const std::lock_guard lock(m_mutex);
        m_accepting = false;
    }
    m_work_available.notify_all();
    m_space_available.notify_all();
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool IoExecutor::is_current_worker() const noexcept {
    return CurrentIoExecutor() == this;
}

bool IoExecutor::dispatch(void (*task)(void*),
                          std::shared_ptr<void> payload,
                          std::string_view operation) {
    return admit({task, std::move(payload), operation}, true);
}

bool IoExecutor::try_dispatch(void (*task)(void*),
                              std::shared_ptr<void> payload,
                              std::string_view operation) {
    return admit({task, std::move(payload), operation}, false);
}

bool IoExecutor::admit(Task task, bool wait_for_space) {
    if (!task.execute) {
        return false;
    }
    {
        std::unique_lock lock(m_mutex);
        if (wait_for_space) {
            m_space_available.wait(lock,
                                   [this] { return !m_accepting || m_size < kQueueCapacity; });
        }
        if (!m_accepting || m_size == kQueueCapacity) {
            return false;
        }
        m_queue[(m_head + m_size) % kQueueCapacity] = std::move(task);
        ++m_size;
    }
    m_work_available.notify_one();
    return true;
}

void IoExecutor::worker_loop() {
    CurrentIoExecutor() = this;
    LUMIN_PROFILE_THREAD("Persistence I/O worker");
    while (true) {
        Task task;
        {
            std::unique_lock lock(m_mutex);
            m_work_available.wait(lock, [this] { return !m_accepting || m_size != 0; });
            if (m_size == 0) {
                CurrentIoExecutor() = nullptr;
                return;
            }
            if (m_throttle_seed != 0) {
                // Match the CPU lane's seeded, clock-free service-order perturbation.
                constexpr std::size_t kThrottleWindow = 8;
                std::uint64_t mixed = m_throttle_seed + 0x9E3779B97F4A7C15ull * (++m_pop_counter);
                mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
                mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
                mixed ^= mixed >> 31;
                const auto offset =
                    static_cast<std::size_t>(mixed % std::min(m_size, kThrottleWindow));
                std::swap(m_queue[m_head], m_queue[(m_head + offset) % kQueueCapacity]);
            }
            task = std::move(m_queue[m_head]);
            m_queue[m_head] = {};
            m_head = (m_head + 1) % kQueueCapacity;
            --m_size;
        }
        m_space_available.notify_one();
        try {
            task.execute(task.payload.get());
        } catch (const std::exception& failure) {
            LUMINUMBRA_CORE_ERROR("IoExecutor worker caught exception: {}", failure.what());
        } catch (...) {
            LUMINUMBRA_CORE_ERROR("IoExecutor worker caught unknown exception.");
        }
    }
}

} // namespace Luminumbra
