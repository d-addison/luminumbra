// The generated include contains the actual production destructor. Test-local
// synchronization types expose the predicate-to-wait window without a production
// header seam. The real mutex and condition variable still perform the handoff.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

struct Schedule {
    std::mutex gate;
    std::condition_variable transition;
    bool sampled_false = false;
    bool release_predicate = false;
    bool waiting = false;
    bool notified = false;
    bool store_owned_mutex = false;
    bool notify_owned_mutex = false;
    bool first_event_was_lock = false;
    bool saw_event = false;
    bool worker_woke = false;
    std::atomic<bool> observe_shutdown{false};
    std::atomic<bool> delivered{false};

    void release(bool from_lock) {
        std::lock_guard<std::mutex> guard(gate);
        if (!saw_event) {
            first_event_was_lock = from_lock;
            saw_event = true;
        }
        release_predicate = true;
        transition.notify_all();
    }
};

class TransitionMutex {
public:
    explicit TransitionMutex(Schedule& schedule)
        : m_schedule(schedule) {}

    void lock() {
        if (m_schedule.observe_shutdown.load()) {
            // A correct destructor reaches this while the worker still owns the
            // task mutex. Let that worker commit its already-false predicate to
            // waiting, then acquire the same real mutex.
            m_schedule.release(true);
        }
        m_mutex.lock();
        std::lock_guard<std::mutex> guard(m_schedule.gate);
        m_owner = std::this_thread::get_id();
    }

    void unlock() {
        {
            std::lock_guard<std::mutex> guard(m_schedule.gate);
            m_owner = {};
        }
        m_mutex.unlock();
    }

    bool owned() const {
        // Caller holds gate, which also protects publication of m_owner.
        return m_owner == std::this_thread::get_id();
    }

private:
    Schedule& m_schedule;
    std::mutex m_mutex;
    std::thread::id m_owner;
};

struct ShutdownFlag {
    Schedule& schedule;
    TransitionMutex& task_mutex;
    std::atomic<bool> value{false};

    void operator=(bool shutdown) {
        std::lock_guard<std::mutex> guard(schedule.gate);
        schedule.store_owned_mutex = task_mutex.owned();
        if (!schedule.saw_event) {
            schedule.saw_event = true;
        }
        value.store(shutdown);
    }
};

struct TaskCondition {
    Schedule& schedule;
    TransitionMutex& task_mutex;
    std::condition_variable_any cv;

    void notify_all() {
        {
            std::lock_guard<std::mutex> guard(schedule.gate);
            schedule.notify_owned_mutex = task_mutex.owned();
            schedule.notified = true;
            schedule.delivered.store(schedule.waiting);
        }
        cv.notify_all();
        // On the broken destructor, notify happens BEFORE the held worker can
        // enter wait. Releasing it now reproduces the lost notification exactly.
        schedule.release(false);
    }
};

class ShutdownFixture {
public:
    explicit ShutdownFixture(Schedule& schedule, bool start_worker)
        : m_task_mutex(schedule)
        , m_shutdown{schedule, m_task_mutex, false}
        , m_task_cv{schedule, m_task_mutex, {}} {
        // At most two thread slots: one nonjoinable slot and one real worker.
        m_worker_threads.emplace_back();
        if (start_worker) {
            m_worker_threads.emplace_back([this, &schedule] {
                std::unique_lock<TransitionMutex> task_lock(m_task_mutex);
                {
                    std::unique_lock<std::mutex> gate_lock(schedule.gate);
                    schedule.sampled_false = !m_shutdown.value.load();
                    schedule.transition.notify_all();
                    // All waits are bounded for failure cleanup on loaded CI.
                    if (!schedule.transition.wait_for(
                            gate_lock, 2s, [&schedule] { return schedule.release_predicate; })) {
                        return;
                    }
                    schedule.waiting = true;
                }
                // The predicate was already observed false while holding the
                // task mutex. Do not re-read shutdown before committing to wait.
                // delivered models only notifications that reach this wait;
                // the predicate handles spurious wakeups without timing oracles.
                schedule.worker_woke = m_task_cv.cv.wait_for(
                    task_lock, 2s, [&schedule] { return schedule.delivered.load(); });
            });
            std::unique_lock<std::mutex> gate_lock(schedule.gate);
            ready = schedule.transition.wait_for(
                gate_lock, 2s, [&schedule] { return schedule.sampled_false; });
        }
        schedule.observe_shutdown.store(true);
    }

    ~ShutdownFixture();
    bool ready = true;

private:
    TransitionMutex m_task_mutex;
    ShutdownFlag m_shutdown;
    TaskCondition m_task_cv;
    std::vector<std::thread> m_worker_threads;
};

#include "audio_shutdown_body.inc"

bool check_shutdown(bool start_worker) {
    Schedule schedule;
    bool ready = false;
    {
        ShutdownFixture fixture(schedule, start_worker);
        ready = fixture.ready;
    }
    const bool passed = ready && schedule.first_event_was_lock && schedule.store_owned_mutex &&
                        schedule.notified && !schedule.notify_owned_mutex &&
                        (!start_worker || schedule.worker_woke);
    if (!passed) {
        std::cerr << "Shutdown failed lock/store/unlock/notify/join handoff (worker="
                  << start_worker << ")\n";
    }
    return passed;
}
} // namespace

int main() {
    // Two finite cases: no running worker and a controlled false-predicate waiter.
    const bool idle = check_shutdown(false);
    const bool transition = check_shutdown(true);
    return idle && transition ? 0 : 1;
}
