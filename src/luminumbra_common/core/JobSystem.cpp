#include "JobSystem.h"
#include "../../../include/luminumbra/core/Types.h"
#include "core/Environment.h"
#include "core/Log.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace Luminumbra {

namespace {

// cache-line size for false-sharing avoidance. The
// completion counter is written by every worker that finishes a job in the
// batch (a contended hot atomic); padding it onto its own line keeps those RMW
// stores off the line that holds the mutex/condition_variable the waiter polls.
#if defined(__cpp_lib_hardware_interference_size)
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;
#endif

} // namespace

// align the completion counter to its own cache
// line so the batch-wide atomic decrement contended by every finishing worker
// does not false-share with `mutex`/`condition` (the line the single waiter
// loads under the lock). `counter` keeps its acquire/release semantics; only
// its placement changes, so the lost-wakeup discipline below is untouched.
#if defined(_MSC_VER)
#pragma warning(push)
// Cache-line isolation intentionally adds layout padding. The assertions below
// keep the diagnostic suppression local to the one type requiring that layout.
#pragma warning(disable : 4324)
#endif
struct JobCompletionState {
    explicit JobCompletionState(int job_count)
        : counter(job_count) {}

    alignas(kCacheLine) std::atomic<int> counter;
    // Padded onto a separate line from the counter above.
    alignas(kCacheLine) std::mutex mutex;
    std::condition_variable condition;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(alignof(JobCompletionState) >= kCacheLine);
static_assert(offsetof(JobCompletionState, mutex) >= kCacheLine);

namespace {

void complete_job(const std::shared_ptr<JobCompletionState>& completion) {
    if (!completion) {
        return;
    }

    if (completion->counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // notify with completion->mutex HELD (the
        // canonical condition_variable idiom) rather than after a momentary empty
        // lock. The waiter (JobSystem::wait) evaluates `counter <= 0` under this
        // mutex before blocking; the counter is an atomic decremented OUTSIDE the
        // mutex, so holding the lock across the notify guarantees the waiter is
        // either pre-predicate (observes 0, never blocks) or already enqueued on
        // the CV (receives the notify) -- the wakeup cannot slip into the gap
        // between the waiter releasing the mutex inside wait and finishing its
        // CV enqueue. Hardening (not the root cause of this task's crash, which
        // was a FastNoise SIMD over-read), kept because it is the correct idiom.
        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->condition.notify_all();
    }
}

} // namespace

// run one pooled slot and complete its batch. The
// completion decrement runs on EVERY exit path (normal return or a throwing
// job) so the batch barrier in wait always drains -- this replaces the
// per-job CompletionGuard lambda the old dispatch_batch wrapped each job in.
// A member (declared in the header) so it can name the private PooledJob type.
void JobSystem::run_slot(PooledJob& slot) {
    // Take ownership locally so the slot can be cleared even if job throws.
    Job job = std::move(slot.job);
    std::shared_ptr<JobCompletionState> completion = std::move(slot.completion);
    slot.job = nullptr;
    slot.completion.reset();

    struct Finisher {
        std::shared_ptr<JobCompletionState> completion;
        ~Finisher() {
            complete_job(completion);
        }
    } finisher{std::move(completion)};

    if (job) {
        job();
    }
}

// ---- PooledQueue --------------------------------------------------------
// a pre-sized ring of PooledJob slots replacing the
// std::queue<Job>. push into an already-grown lane and pop are both
// allocation-free; growth doubles capacity (never shrinks) so steady-state
// dispatch reuses slots. All calls are serialized by JobSystem::m_queue_mutex.

// Initial slot count per lane. Sized so typical streaming batches (chunk
// generation / meshing) fit without an early growth; growth still handles
// bursts.
namespace {
constexpr std::size_t kInitialPoolCapacity = 256;
}

JobSystem::PooledQueue::PooledQueue() {
    m_slots.resize(kInitialPoolCapacity);
}

void JobSystem::PooledQueue::grow() {
    const std::size_t old_cap = m_slots.empty() ? 0 : m_slots.size();
    const std::size_t new_cap = old_cap == 0 ? kInitialPoolCapacity : old_cap * 2;

    std::vector<PooledJob> grown;
    grown.resize(new_cap);
    // Re-linearize the live slots [head, head+size) into the front of the new
    // buffer so head=0 after growth. An empty pool holds no live slots, so the
    // modulo by old_cap only runs when old_cap is non-zero.
    if (old_cap != 0) {
        for (std::size_t i = 0; i < m_size; ++i) {
            grown[i] = std::move(m_slots[(m_head + i) % old_cap]);
        }
    }
    m_slots = std::move(grown);
    m_head = 0;
    m_tail = m_size;
}

void JobSystem::PooledQueue::push(PooledJob&& slot) {
    if (m_size == m_slots.size()) {
        grow();
    }
    m_slots[m_tail] = std::move(slot);
    m_tail = (m_tail + 1) % m_slots.size();
    ++m_size;
}

JobSystem::PooledJob JobSystem::PooledQueue::pop() {
    // Caller guarantees !empty.
    PooledJob out = std::move(m_slots[m_head]);
    // Release any state the vacated slot still references so a long-lived ring
    // does not pin shared_ptrs / lambda captures between reuses.
    m_slots[m_head].job = nullptr;
    m_slots[m_head].completion.reset();
    m_head = (m_head + 1) % m_slots.size();
    --m_size;
    return out;
}

JobSystem::PooledJob JobSystem::PooledQueue::pop_at(std::size_t offset) {
    // Caller guarantees offset < size. Swap the selected slot to the front
    // and pop — the displaced front job is served later (the  service-
    // order perturbation); ring invariants (head/tail/size) are untouched.
    const std::size_t index = (m_head + offset) % m_slots.size();
    if (index != m_head) {
        std::swap(m_slots[index], m_slots[m_head]);
    }
    return pop();
}

// ---- JobSystem ----------------------------------------------------------

JobSystem::~JobSystem() {
    shutdown();
}

void JobSystem::startup(std::size_t worker_count) {
    std::size_t num_threads = worker_count;
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
    }
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
        m_consecutive_high_served = 0;

        // opt-in adversarial job-service
        // throttle. Read ONCE at startup (mirrors LUMINUMBRA_JOB_WORKERS);
        // unset/empty/0 = off = one predictable branch at the pop site.
        m_throttle_enabled = false;
        m_throttle_seed = 0;
        m_throttle_pop_counter = 0;
        if (const auto throttle = Core::ReadEnvironment("LUMINUMBRA_JOB_THROTTLE")) {
            const std::uint64_t seed = std::strtoull(throttle->c_str(), nullptr, 10);
            if (seed != 0) {
                m_throttle_enabled = true;
                m_throttle_seed = seed;
            }
        }
    }

    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i) {
        m_workers.emplace_back(&JobSystem::worker_loop, this);
    }
    if (m_throttle_enabled) {
        LUMINUMBRA_CORE_INFO("JobSystem started with " + std::to_string(num_threads) +
                             " threads (throttle armed, seed " + std::to_string(m_throttle_seed) +
                             ").");
    } else {
        LUMINUMBRA_CORE_INFO("JobSystem started with " + std::to_string(num_threads) + " threads.");
    }
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

JobSystem::PooledQueue& JobSystem::queue_for(JobPriority priority) {
    return priority == JobPriority::High ? m_high_queue : m_normal_queue;
}

JobSystem::PooledJob JobSystem::throttled_pop(PooledQueue& queue) {
    // Caller holds m_queue_mutex and guarantees !queue.empty.
    if (!m_throttle_enabled) {
        return queue.pop();
    }
    // SplitMix64 over (seed, pop counter): a seeded, wall-clock-free selection
    // from a small window at the head. Adversarial to service order (
    // the throttle must REORDER, not merely slow) while
    // repeatable for a given seed and arrival sequence. The matrix asserts sim
    // hashes are INVARIANT under it.
    constexpr std::size_t kThrottleWindow = 8;
    std::uint64_t x = m_throttle_seed + 0x9E3779B97F4A7C15ull * (++m_throttle_pop_counter);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    const std::size_t window = std::min(queue.size(), kThrottleWindow);
    return queue.pop_at(static_cast<std::size_t>(x % window));
}

void JobSystem::dispatch(Job job, JobPriority priority) {
    if (!job) {
        return;
    }

    bool accepted = false;
    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            // store the caller's Job directly in a
            // pooled slot (no second std::function wrapper). Single dispatch has
            // no completion handle.
            queue_for(priority).push(PooledJob{std::move(job), nullptr});
            accepted = true;
        }
    }

    if (!accepted) {
        LUMINUMBRA_CORE_WARN("JobSystem rejected job dispatch while shutting down.");
        return;
    }

    m_condition.notify_one();
}

JobHandle JobSystem::dispatch_batch(const std::vector<Job>& jobs, JobPriority priority) {
    if (jobs.empty()) {
        return JobHandle{};
    }

    auto completion = std::make_shared<JobCompletionState>(static_cast<int>(jobs.size()));
    JobHandle handle{std::shared_ptr<std::atomic<int>>(completion, &completion->counter),
                     completion};
    bool accepted = false;

    {
        std::unique_lock<std::mutex> lock(m_queue_mutex);
        if (m_accepting_jobs && !m_stop_threads.load(std::memory_order_acquire)) {
            PooledQueue& queue = queue_for(priority);
            for (const auto& job : jobs) {
                // each slot carries the caller's Job
                // and a shared completion handle. The worker decrements the
                // counter via run_slot's Finisher on every exit path -- the old
                // per-job CompletionGuard lambda (which forced a heap-allocated
                // wrapper std::function) is gone.
                queue.push(PooledJob{job, completion});
            }
            accepted = true;
        }
    }

    if (!accepted) {
        // same lost-wakeup discipline as
        // complete_job -- drop the counter to 0 and notify with completion->mutex
        // HELD so a waiter that called wait concurrently with this rejection
        // cannot miss the wakeup.
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->counter.store(0, std::memory_order_release);
            completion->condition.notify_all();
        }
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
    stats.high_priority_queue_depth = m_high_queue.size();
    stats.normal_priority_queue_depth = m_normal_queue.size();
    stats.queue_depth = stats.high_priority_queue_depth + stats.normal_priority_queue_depth;
    stats.accepting_jobs = m_accepting_jobs;
    stats.stop_requested = m_stop_threads.load(std::memory_order_acquire);
    return stats;
}

void JobSystem::worker_loop() {
    while (true) {
        PooledJob slot;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_condition.wait(lock, [this] {
                return !m_high_queue.empty() || !m_normal_queue.empty() || m_stop_threads.load();
            });

            if (m_stop_threads.load() && m_high_queue.empty() && m_normal_queue.empty()) {
                return;
            }

            // Prefer the High lane, but serve a Normal job after
            // kNormalServiceInterval consecutive High jobs so a sustained
            // High backlog cannot starve Normal work. The counter also
            // accumulates while only High work exists, so Normal work that
            // arrives behind a long High burst is served promptly.
            const bool high_available = !m_high_queue.empty();
            const bool normal_available = !m_normal_queue.empty();
            const bool serve_normal =
                normal_available &&
                (!high_available || m_consecutive_high_served >= kNormalServiceInterval);

            if (serve_normal) {
                slot = throttled_pop(m_normal_queue);
                m_consecutive_high_served = 0;
            } else {
                slot = throttled_pop(m_high_queue);
                ++m_consecutive_high_served;
            }
        }

        // run outside the queue lock. run_slot
        // completes the batch (Finisher) on both the normal and throwing paths,
        // so the completion barrier in wait drains exactly as before.
        try {
            run_slot(slot);
        } catch (const std::exception& exception) {
            LUMINUMBRA_CORE_ERROR("JobSystem worker caught job exception: " +
                                  std::string(exception.what()));
        } catch (...) {
            LUMINUMBRA_CORE_ERROR("JobSystem worker caught unknown job exception.");
        }
    }
}

} // namespace Luminumbra
