#pragma once

#include "../core/JobSystem.h"

#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace Luminumbra {

namespace Persistence::detail {
// Save transaction ownership is thread-local, including recursive lock ownership.
bool CurrentThreadOwnsSaveTransaction();
} // namespace Persistence::detail

class WorkerBoundaryUnavailable : public std::runtime_error {
public:
    WorkerBoundaryUnavailable()
        : std::runtime_error("Worker boundary admission refused") {}
};

// The owner must hold no locks needed by work and must keep its inputs stable.
// Waiting here preserves the caller's deterministic publication boundary. There
// is at most one outstanding task per waiting owner, with no polling or copying
// of world containers. Explicit enclosing maintenance transactions must retain
// their thread-local ownership. A null pool supports standalone worlds only.
template<typename Work>
void RunWorkerBoundary(JobSystem* jobs, std::string_view operation, const Work& work) {
    if (!jobs || Persistence::detail::CurrentThreadOwnsSaveTransaction()) {
        work();
        return;
    }
    // Already off the owner; queuing and waiting here can exhaust this pool.
    if (jobs->is_current_worker() || jobs->io_executor().is_current_worker()) {
        work();
        return;
    }
    struct Task {
        const Work& work;
        std::promise<void> completion;

        static void Execute(void* payload) {
            auto& task = *static_cast<Task*>(payload);
            try {
                task.work();
                task.completion.set_value();
            } catch (...) {
                task.completion.set_exception(std::current_exception());
            }
        }
    };
    const auto task = std::make_shared<Task>(Task{work, {}});
    auto completed = task->completion.get_future();
    // Bounded admission applies backpressure, never an inline fallback when
    // stopped. Completion still fences every caller-owned input and result.
    if (!jobs->io_executor().dispatch(&Task::Execute, task, operation)) {
        throw WorkerBoundaryUnavailable();
    }
    completed.get();
}

} // namespace Luminumbra
