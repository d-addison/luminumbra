#pragma once

// =============================================================================
// PixelIo — CPU-side pixel-buffer image writing shared by every capture path.
// =============================================================================
// Render-only artifact output: photo mode (DebugOverlays), the menu-backdrop
// screenshot helpers (MenuScreens), the --survey capture mode (SceneSurvey),
// the timelapse writer in main_client, and the QA scenario harness's pixel
// captures all funnel through this writer. Lives in luminumbra_client (not the
// QA harness) because the shipping client's own capture features depend on it.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <string_view>
#include <vector>

namespace luminumbra::game {
struct PhotoSidecar;
}

namespace Luminumbra::Rendering {

// All capture buffers are bounded by 64M pixels. Check before allocation or
// indexing, including on 32-bit hosts; bytes must fit stream/GL signed sizes.
inline bool CapturePixelBytes(int width, int height, std::size_t channels, std::size_t& bytes) {
    constexpr std::size_t kMaxPixels = 64u * 1024u * 1024u;
    if (width <= 0 || height <= 0 || channels == 0)
        return false;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > kMaxPixels / h)
        return false;
    const auto pixels = w * h;
    if (channels > static_cast<std::size_t>(std::numeric_limits<int>::max()) / pixels)
        return false;
    bytes = pixels * channels;
    return true;
}

// Flushing is part of artifact success; close may discover additional failures.
inline bool FlushCaptureStream(std::ostream& output) {
    output.flush();
    return static_cast<bool>(output);
}
// Also accepts other owning output streams with flush(), close() and stream state.
template<typename OutputFile>
inline bool CloseCaptureFile(OutputFile& output) {
    const bool flushed = FlushCaptureStream(output);
    output.close();
    return flushed && static_cast<bool>(output);
}

inline bool WriteCaptureStream(std::ostream& output, std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return FlushCaptureStream(output);
}

bool WriteCaptureText(const std::filesystem::path& path, std::string_view text);

// Write an RGB8 buffer (glReadPixels order: bottom row first, tightly packed,
// 3 bytes per pixel) to `path` as a binary "P6" PPM, flipping vertically so
// the file reads top-down. Creates the parent directory. Returns false (and
// logs) on an invalid size or an I/O failure.
bool WritePixelBufferPpm(const std::filesystem::path& path,
                         int width,
                         int height,
                         const std::vector<unsigned char>& pixels);

// Stream serializers share the same validation and write checks as file output.
bool WritePixelBufferPpmStream(std::ostream& output,
                               int width,
                               int height,
                               const std::vector<unsigned char>& pixels);
bool WritePixelBufferThumbnailTgaStream(std::ostream& output,
                                        int width,
                                        int height,
                                        const std::vector<unsigned char>& pixels,
                                        int max_dimension);

// Complete a group of owning streams, checking every open, write, flush and
// close. Close every opened output even when an earlier operation failed, then
// invoke the publisher only when all outputs completed.
template<typename OutputFile, typename Publish>
bool WritePhotoCaptureFiles(OutputFile& image,
                            OutputFile& thumbnail,
                            OutputFile& sidecar,
                            int width,
                            int height,
                            const std::vector<unsigned char>& pixels,
                            std::string_view metadata,
                            const Publish& publish) {
    const bool opened = image && thumbnail && sidecar;
    const bool written =
        opened && WritePixelBufferPpmStream(image, width, height, pixels) &&
        WritePixelBufferThumbnailTgaStream(thumbnail, width, height, pixels, 512) &&
        WriteCaptureStream(sidecar, metadata);
    const bool image_closed = CloseCaptureFile(image);
    const bool thumbnail_closed = CloseCaptureFile(thumbnail);
    const bool sidecar_closed = CloseCaptureFile(sidecar);
    return written && image_closed && thumbnail_closed && sidecar_closed && publish();
}

// Required photo artifacts use exclusively reserved directories. A successful
// capture publishes its completion directory only after every output has closed.
// Returns the reserved positive id, or zero on refusal. Never removes old files.
int PersistPhotoCapture(const std::filesystem::path& root,
                        int first_id,
                        int width,
                        int height,
                        const std::vector<unsigned char>& pixels,
                        const luminumbra::game::PhotoSidecar& side);
// Legacy flat files are admitted only when every required artifact is complete.
bool ReadCompletedPhotoCapture(const std::filesystem::path& root,
                               int id,
                               int& stars,
                               bool legacy = false);
bool WritePixelBufferThumbnailTga(const std::filesystem::path& path,
                                  int width,
                                  int height,
                                  const std::vector<unsigned char>& pixels,
                                  int max_dimension);

} // namespace Luminumbra::Rendering
