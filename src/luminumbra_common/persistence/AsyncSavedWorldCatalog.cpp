#include "AsyncSavedWorldCatalog.h"
#include "core/Profiler.h"

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace Luminumbra::Persistence {

struct AsyncSavedWorldCatalog::Impl {
    struct Request {
        Scan scan;
        std::uint64_t generation;
        std::stop_source cancellation;
    };

    std::mutex mutex;
    std::condition_variable_any wake;
    std::optional<Request> pending;
    std::optional<SavedWorldCatalog> ready;
    std::stop_source active;
    std::uint64_t generation = 0;
    std::jthread worker;

    void Run(const std::stop_token& stop) {
        LUMIN_PROFILE_THREAD("Saved world catalog worker");
        LUMIN_PROFILE_LOCK(std::unique_lock, lock, mutex, "Saved world catalog lock wait");
        while (LUMIN_PROFILE_WAIT("Saved world catalog condition wait",
                                  wake.wait(lock, stop, [this] { return pending.has_value(); }))) {
            if (!pending.has_value())
                continue;
            Request request = std::move(pending.value());
            pending.reset();
            active = request.cancellation;
            lock.unlock();
            SavedWorldCatalog result;
            try {
                LUMIN_PROFILE_ZONE_N("Saved world catalog worker job");
                result = request.scan(request.cancellation.get_token());
            } catch (const std::exception& e) {
                result.error = std::string("Saved worlds unavailable: ") + e.what();
            } catch (...) {
                result.error = "Saved worlds unavailable: validation failed.";
            }
            LUMIN_PROFILE_WAIT("Saved world catalog lock wait", lock.lock());
            if (request.generation == generation && !stop.stop_requested() &&
                !request.cancellation.stop_requested())
                ready = std::move(result);
        }
    }
};

AsyncSavedWorldCatalog::AsyncSavedWorldCatalog()
    : m_impl(std::make_unique<Impl>()) {}

AsyncSavedWorldCatalog::~AsyncSavedWorldCatalog() {
    Cancel();
    m_impl->worker.request_stop();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
}

void AsyncSavedWorldCatalog::Request(Scan scan) {
    LUMIN_PROFILE_LOCK(std::lock_guard, lock, m_impl->mutex, "Saved world catalog lock wait");
    m_impl->active.request_stop();
    auto& pending = m_impl->pending;
    if (pending.has_value())
        pending->cancellation.request_stop();
    m_impl->ready.reset();
    m_impl->pending = Impl::Request{std::move(scan), ++m_impl->generation, std::stop_source{}};
    if (!m_impl->worker.joinable())
        m_impl->worker = std::jthread([this](const std::stop_token& stop) { m_impl->Run(stop); });
    m_impl->wake.notify_one();
}

void AsyncSavedWorldCatalog::Cancel() {
    LUMIN_PROFILE_LOCK(std::lock_guard, lock, m_impl->mutex, "Saved world catalog lock wait");
    ++m_impl->generation;
    m_impl->active.request_stop();
    auto& pending = m_impl->pending;
    if (pending.has_value())
        pending->cancellation.request_stop();
    m_impl->pending.reset();
    m_impl->ready.reset();
}

std::optional<SavedWorldCatalog> AsyncSavedWorldCatalog::Poll() {
    LUMIN_PROFILE_LOCK(std::lock_guard, lock, m_impl->mutex, "Saved world catalog lock wait");
    return std::exchange(m_impl->ready, std::nullopt);
}

} // namespace Luminumbra::Persistence
