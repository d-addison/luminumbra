#include "app/RegisterDeveloperWidgets.h"

#include "debug/DeveloperConsole.h"
#include "rendering/passes/DebugViewPass.h"

namespace Luminumbra::Client::App {
std::vector<Debug::WidgetDef> RegisterDeveloperWidgets(const DeveloperWidgetServices& services) {
    auto definitions = Debug::BuiltinDeveloperWidgets();
    for (auto& def : definitions) {
        if (def.id == "console.help" || def.presentation == Debug::Presentation::ActionOnly)
            continue; // The console supplies its own generated help adapter.
        const auto service = services.find(def.id);
        if (service == services.end() || !service->second.contents) {
            const auto title = def.title;
            def.available = [title] {
                return Debug::Availability{false, title + " service unavailable"};
            };
            continue;
        }
        def.available = service->second.available;
        def.visibility_changed = service->second.visibility_changed;
        if (def.id == "render.debug_views") {
            def.visibility_changed = [changed = def.visibility_changed](bool visible) {
                if (!visible) {
                    Rendering::DebugViewPass::controls().inspector_open = false;
                }
                if (changed) {
                    changed(visible);
                }
            };
        }
        // Pass immutable metadata and contents to the UI host; it owns window/focus handling.
        const auto panel = def;
        def.draw = [panel,
                    contents = service->second.contents](Debug::DeveloperFrameAdapter& frame) {
            if (frame.draw)
                frame.draw(panel, contents);
        };
    }
    return definitions;
}
} // namespace Luminumbra::Client::App
