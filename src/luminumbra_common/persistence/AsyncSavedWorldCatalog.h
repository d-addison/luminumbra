#pragma once

#include "SavedWorldCatalog.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace Luminumbra::Persistence {

// One worker and one coalesced pending request. Callers use Request/Cancel/Poll
// from their owning thread; callbacks run off-thread and must honor cancellation
// and own their inputs. No callback may access UI or live simulation state.
class AsyncSavedWorldCatalog {
public:
    using Scan = std::function<SavedWorldCatalog(std::stop_token)>;

    AsyncSavedWorldCatalog();
    ~AsyncSavedWorldCatalog();
    AsyncSavedWorldCatalog(const AsyncSavedWorldCatalog&) = delete;
    AsyncSavedWorldCatalog& operator=(const AsyncSavedWorldCatalog&) = delete;

    void Request(Scan scan);
    void Cancel();
    std::optional<SavedWorldCatalog> Poll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Luminumbra::Persistence
