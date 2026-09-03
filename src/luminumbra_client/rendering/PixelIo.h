#pragma once

// =============================================================================
// PixelIo — CPU-side pixel-buffer image writing shared by every capture path.
// =============================================================================
// Render-only artifact output: photo mode (DebugOverlays), the menu-backdrop
// screenshot helpers (MenuScreens), the --survey capture mode (SceneSurvey),
// the timelapse writer in main_client, and the QA scenario harness's pixel
// captures all funnel through this writer. Lives in luminumbra_client (not the
// QA harness) because the shipping client's own capture features depend on it.

#include <filesystem>
#include <vector>

namespace Luminumbra::Rendering {

// Write an RGB8 buffer (glReadPixels order: bottom row first, tightly packed,
// 3 bytes per pixel) to `path` as a binary "P6" PPM, flipping vertically so
// the file reads top-down. Creates the parent directory. Returns false (and
// logs) on an invalid size or an I/O failure.
bool WritePixelBufferPpm(const std::filesystem::path& path,
                         int width,
                         int height,
                         const std::vector<unsigned char>& pixels);

} // namespace Luminumbra::Rendering
