#pragma once

#include "debug/DeveloperConsole.h"

namespace Luminumbra::Client::Debug {
void PrepareDeveloperPanel(const std::optional<DeveloperPanelLayout>& layout,
                           float width,
                           float height);
std::optional<DeveloperPanelLayout> CaptureDeveloperPanel(bool open);
void DrawDeveloperWidget(DeveloperWidgetRegistry& widgets,
                         DeveloperFrameAdapter& frame,
                         const WidgetDef& def,
                         const DrawFn& contents);
} // namespace Luminumbra::Client::Debug
