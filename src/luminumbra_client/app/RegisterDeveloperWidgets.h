#pragma once

#include "debug/DeveloperWidgetRegistry.h"

#include <map>

namespace Luminumbra::Client::App {
// Service callbacks resolve the current owners on invocation. No graphics dependencies:
// tests may supply counters instead of the client adapters.
struct DeveloperWidgetService {
    Debug::AvailabilityFn available = {};
    Debug::DrawFn contents = {};
    std::function<void(bool)> visibility_changed = {};
};
using DeveloperWidgetServices = std::map<std::string, DeveloperWidgetService>;
std::vector<Debug::WidgetDef> RegisterDeveloperWidgets(const DeveloperWidgetServices& services);
} // namespace Luminumbra::Client::App
