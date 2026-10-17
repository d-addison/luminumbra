#include "rendering/PixelIo.h"

#include "core/Log.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>

namespace Luminumbra::Rendering {

bool WritePixelBufferPpm(const std::filesystem::path& path,
                         int width,
                         int height,
                         const std::vector<unsigned char>& pixels) {
    std::size_t bytes = 0;
    if (!CapturePixelBytes(width, height, 3, bytes) || pixels.size() != bytes) {
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LUMINUMBRA_CORE_ERROR("Failed to create screenshot directory '{}': {}",
                              path.parent_path().string(),
                              ec.message());
        return false;
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to write screenshot artifact: {}", path.string());
        return false;
    }

    output << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int row = height - 1; row >= 0; --row) {
        const std::size_t offset = static_cast<std::size_t>(row) * row_stride;
        output.write(reinterpret_cast<const char*>(pixels.data() + offset),
                     static_cast<std::streamsize>(row_stride));
    }
    return CloseCaptureFile(output);
}

bool WriteCaptureText(const std::filesystem::path& path, std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        return false;
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    const bool written = WriteCaptureStream(output, text);
    const bool closed = CloseCaptureFile(output);
    return written && closed;
}

} // namespace Luminumbra::Rendering
