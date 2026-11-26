#pragma once

namespace Luminumbra::Test {

// The common test executable's allocation replacements call this observer.
// It is thread-local so a blocked allocation never stalls unrelated workers.
struct AllocationObserver {
    void (*callback)(void*) = nullptr;
    void* context = nullptr;
};

inline AllocationObserver& CurrentAllocationObserver() {
    static thread_local AllocationObserver observer;
    return observer;
}

inline void ObserveAllocation() {
    const auto observer = CurrentAllocationObserver();
    if (observer.callback != nullptr)
        observer.callback(observer.context);
}

class ScopedAllocationObserver {
public:
    ScopedAllocationObserver(void (*callback)(void*), void* context)
        : m_previous(CurrentAllocationObserver()) {
        CurrentAllocationObserver() = {.callback = callback, .context = context};
    }
    ~ScopedAllocationObserver() {
        CurrentAllocationObserver() = m_previous;
    }
    ScopedAllocationObserver(const ScopedAllocationObserver&) = delete;
    ScopedAllocationObserver& operator=(const ScopedAllocationObserver&) = delete;

private:
    AllocationObserver m_previous;
};
} // namespace Luminumbra::Test
