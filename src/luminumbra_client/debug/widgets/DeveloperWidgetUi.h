#pragma once

#include "debug/DeveloperConsole.h"

namespace Luminumbra::Client::Debug {
void DrawDeveloperWidget(DeveloperWidgetRegistry& widgets,
                         DeveloperFrameAdapter& frame,
                         const WidgetDef& def,
                         const DrawFn& contents);
} // namespace Luminumbra::Client::Debug
