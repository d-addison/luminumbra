#pragma once

#include "debug/DeveloperConsole.h"

namespace Luminumbra::Client::Debug {
void DrawDebugViewContents(DeveloperConsole& console, int mode);
void DrawWireframeContents(DeveloperConsole& console, bool enabled);
void DrawTimeControlContents(DeveloperConsole& console, float scale);
} // namespace Luminumbra::Client::Debug
