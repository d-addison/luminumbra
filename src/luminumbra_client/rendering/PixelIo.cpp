#include "rendering/PixelIo.h"

#include "core/Log.h"

#include <cstddef>
#include <fstream>
#include <ios>
#include <system_error>

namespace Luminumbra::Rendering {

bool WritePixelBufferPpm(const std::filesystem::path& path,
                         int width,
                         int height,
                         const std::vector<unsigned char>& pixels) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
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
    return true;
}

} // namespace Luminumbra::Rendering
