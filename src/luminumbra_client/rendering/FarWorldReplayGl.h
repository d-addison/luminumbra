#pragma once
#include "FarWorldReplay.h"
#include <memory>
namespace Luminumbra::Rendering {
// Implemented only in the GL client, never linked into common_tests.
std::unique_ptr<ReplayStateAccess> MakeFarWorldReplayGlAccess();
} // namespace Luminumbra::Rendering
