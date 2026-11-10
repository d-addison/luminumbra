#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

namespace Luminumbra::Client::UiTexture {

inline std::size_t RgbaByteCount(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    // Keep byte extents representable by both size_t and contiguous-container differences.
    constexpr auto max_bytes = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (columns > max_bytes / 4 / rows) {
        return 0;
    }
    return columns * rows * 4;
}

inline bool ValidRgbaData(const unsigned char* pixels, std::size_t size, int width, int height) {
    const auto expected_size = RgbaByteCount(width, height);
    return expected_size != 0 && pixels != nullptr && size == expected_size;
}

struct TgaImage {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
};

namespace Detail {
constexpr std::size_t header_size = 18;

struct TgaLayout {
    int width = 0;
    int height = 0;
    std::size_t pixel_bytes = 0;
    std::size_t offset = 0;
    std::size_t payload_size = 0;
    std::size_t rgba_size = 0;
};

inline TgaLayout ParseHeader(std::span<const unsigned char> header, std::size_t file_size) {
    if (header.size() < header_size || file_size < header_size) {
        return {};
    }
    const auto depth = header[16];
    const auto attributes = header[17] & 0x0f;
    // Only uncompressed, non-interleaved, left-origin true-colour images are supported.
    if (header[1] != 0 || header[2] != 2 || (depth != 24 && depth != 32) ||
        (header[17] & 0xd0) != 0 || (attributes != 0 && (depth != 32 || attributes != 8)) ||
        std::any_of(
            header.begin() + 3, header.begin() + 8, [](unsigned char byte) { return byte != 0; })) {
        return {};
    }
    const int width = header[12] | (static_cast<int>(header[13]) << 8);
    const int height = header[14] | (static_cast<int>(header[15]) << 8);
    const auto rgba_size = RgbaByteCount(width, height);
    if (rgba_size == 0) {
        return {};
    }
    const std::size_t pixel_bytes = depth / 8;
    const std::size_t payload_size = rgba_size / 4 * pixel_bytes;
    const std::size_t offset = header_size + header[0];
    if (offset > file_size || payload_size > file_size - offset) {
        return {};
    }
    return {.width = width,
            .height = height,
            .pixel_bytes = pixel_bytes,
            .offset = offset,
            .payload_size = payload_size,
            .rgba_size = rgba_size};
}
} // namespace Detail

// Refusal returns an empty image. Successful pixels are top-origin, premultiplied RGBA.
inline TgaImage DecodeTga(std::span<const unsigned char> bytes) {
    const auto layout = Detail::ParseHeader(bytes, bytes.size());
    if (layout.rgba_size == 0) {
        return {};
    }
    // Both allocations and the pixel loop are bounded by the validated 16-bit TGA dimensions.
    TgaImage image{.pixels = std::vector<unsigned char>(layout.rgba_size),
                   .width = layout.width,
                   .height = layout.height};
    const auto width = static_cast<std::size_t>(layout.width);
    const auto height = static_cast<std::size_t>(layout.height);
    for (std::size_t y = 0; y < height; ++y) {
        const auto dest_y = (bytes[17] & 0x20) != 0 ? y : height - y - 1;
        for (std::size_t x = 0; x < width; ++x) {
            const auto source = layout.offset + (y * width + x) * layout.pixel_bytes;
            const auto dest = (dest_y * width + x) * 4;
            const unsigned char alpha = layout.pixel_bytes == 4 ? bytes[source + 3] : 255;
            image.pixels[dest] = static_cast<unsigned char>(bytes[source + 2] * alpha / 255);
            image.pixels[dest + 1] = static_cast<unsigned char>(bytes[source + 1] * alpha / 255);
            image.pixels[dest + 2] = static_cast<unsigned char>(bytes[source] * alpha / 255);
            image.pixels[dest + 3] = alpha;
        }
    }
    return image;
}

// Uses the RmlUi file-interface contract without depending on RmlUi or a GL context.
template<typename FileInterface, typename Path>
TgaImage LoadTga(FileInterface& files, const Path& path) {
    const auto handle = files.Open(path);
    if (!handle) {
        return {};
    }
    struct FileOwner {
        FileInterface& file_interface;
        decltype(handle) file;
        ~FileOwner() {
            file_interface.Close(file);
        }
    } owner{files, handle};

    if (!files.Seek(handle, 0, SEEK_END)) {
        return {};
    }
    const auto file_size = files.Tell(handle);
    if (file_size == std::numeric_limits<std::size_t>::max() || !files.Seek(handle, 0, SEEK_SET)) {
        return {};
    }
    std::array<unsigned char, Detail::header_size> header{};
    if (files.Read(header.data(), header.size(), handle) != header.size()) {
        return {};
    }
    const auto layout = Detail::ParseHeader(header, file_size);
    if (layout.rgba_size == 0) {
        return {};
    }
    // Read only the validated header, image ID and pixel payload; ignore any trailing footer.
    std::vector<unsigned char> bytes(layout.offset + layout.payload_size);
    std::copy(header.begin(), header.end(), bytes.begin());
    const auto remaining = bytes.size() - header.size();
    if (files.Read(bytes.data() + header.size(), remaining, handle) != remaining) {
        return {};
    }
    return DecodeTga(bytes);
}

} // namespace Luminumbra::Client::UiTexture
