#include "rendering/PixelIo.h"

#include "core/Log.h"

#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/game/PhotoMode.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Luminumbra::Rendering {
namespace {

// Interactive outputs are created exclusively, including their leaf files.
// A collision, link or replaced parent can never truncate somebody else's file.
class FreshCaptureBuffer final : public std::streambuf {
public:
    explicit FreshCaptureBuffer(const std::filesystem::path& path) {
#if defined(_WIN32)
        int descriptor = -1;
        if (_wsopen_s(&descriptor,
                      path.c_str(),
                      _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                      _SH_DENYRW,
                      _S_IREAD | _S_IWRITE) == 0) {
            m_file = _fdopen(descriptor, "wb");
            if (!m_file)
                _close(descriptor);
        }
#else
        const int descriptor =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor >= 0) {
            m_file = ::fdopen(descriptor, "wb");
            if (!m_file)
                ::close(descriptor);
        }
#endif
    }
    ~FreshCaptureBuffer() override {
        close();
    }
    bool is_open() const {
        return m_file != nullptr;
    }
    bool close() {
        auto* file = std::exchange(m_file, nullptr);
        return file && std::fclose(file) == 0;
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (!m_file || count < 0)
            return 0;
        return static_cast<std::streamsize>(
            std::fwrite(data, 1, static_cast<std::size_t>(count), m_file));
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        return m_file && std::fputc(traits_type::to_char_type(value), m_file) != EOF
                   ? value
                   : traits_type::eof();
    }
    int sync() override {
        return m_file ? std::fflush(m_file) : -1;
    }

private:
    std::FILE* m_file = nullptr;
};

class FreshCaptureFile final : public std::ostream {
public:
    explicit FreshCaptureFile(const std::filesystem::path& path)
        : std::ostream(nullptr)
        , m_buffer(path) {
        rdbuf(&m_buffer);
        if (!m_buffer.is_open())
            setstate(std::ios::failbit);
    }
    void close() {
        if (!m_buffer.close())
            setstate(std::ios::failbit);
    }

private:
    FreshCaptureBuffer m_buffer;
};

} // namespace

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

    const bool written = WritePixelBufferPpmStream(output, width, height, pixels);
    const bool closed = CloseCaptureFile(output);
    if (!written || !closed) {
        LUMINUMBRA_CORE_ERROR("Failed to finish screenshot artifact: {}", path.string());
        return false;
    }
    return true;
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

bool WritePixelBufferPpmStream(std::ostream& output,
                               int width,
                               int height,
                               const std::vector<unsigned char>& pixels) {
    std::size_t bytes = 0;
    if (!CapturePixelBytes(width, height, 3, bytes) || pixels.size() != bytes)
        return false;
    output << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int row = height - 1; row >= 0; --row) {
        const std::size_t offset = static_cast<std::size_t>(row) * row_stride;
        output.write(reinterpret_cast<const char*>(pixels.data() + offset),
                     static_cast<std::streamsize>(row_stride));
    }
    return FlushCaptureStream(output);
}

bool WritePixelBufferThumbnailTga(const std::filesystem::path& path,
                                  int srcW,
                                  int srcH,
                                  const std::vector<unsigned char>& rgb,
                                  int maxDim) {
    std::size_t bytes = 0;
    if (!CapturePixelBytes(srcW, srcH, 3, bytes) || rgb.size() != bytes || maxDim <= 0 ||
        maxDim > 65535)
        return false;
    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;
    const bool written = WritePixelBufferThumbnailTgaStream(output, srcW, srcH, rgb, maxDim);
    const bool closed = CloseCaptureFile(output);
    return written && closed;
}

// Bottom-up RGB input maps directly to bottom-up TGA rows. Nearest-neighbour
// downscaling bounds the interactive gallery thumbnail to 512 by 512 pixels.
bool WritePixelBufferThumbnailTgaStream(
    std::ostream& out, int srcW, int srcH, const std::vector<unsigned char>& rgb, int maxDim) {
    std::size_t source_bytes = 0;
    if (!CapturePixelBytes(srcW, srcH, 3, source_bytes) || rgb.size() != source_bytes ||
        maxDim <= 0 || maxDim > 65535)
        return false;
    const float scale =
        std::min(1.0f, static_cast<float>(maxDim) / static_cast<float>(std::max(srcW, srcH)));
    const int dstW = std::max(1, static_cast<int>(static_cast<float>(srcW) * scale));
    const int dstH = std::max(1, static_cast<int>(static_cast<float>(srcH) * scale));
    std::size_t thumbnail_bytes = 0;
    if (!CapturePixelBytes(dstW, dstH, 3, thumbnail_bytes))
        return false;
    std::vector<unsigned char> bgr(thumbnail_bytes);
    for (int y = 0; y < dstH; ++y) {
        const int sy =
            std::min(srcH - 1, static_cast<int>((static_cast<float>(y) + 0.5f) / dstH * srcH));
        for (int x = 0; x < dstW; ++x) {
            const int sx =
                std::min(srcW - 1, static_cast<int>((static_cast<float>(x) + 0.5f) / dstW * srcW));
            const unsigned char* s = &rgb[(static_cast<std::size_t>(sy) * srcW + sx) * 3u];
            unsigned char* d = &bgr[(static_cast<std::size_t>(y) * dstW + x) * 3u];
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0]; // RGB -> BGR
        }
    }
    unsigned char hdr[18] = {0};
    hdr[2] = 2; // uncompressed true-color
    hdr[12] = static_cast<unsigned char>(dstW & 0xFF);
    hdr[13] = static_cast<unsigned char>((dstW >> 8) & 0xFF);
    hdr[14] = static_cast<unsigned char>(dstH & 0xFF);
    hdr[15] = static_cast<unsigned char>((dstH >> 8) & 0xFF);
    hdr[16] = 24; // bits per pixel
    hdr[17] = 0;  // descriptor: bottom-up origin (matches the GL buffer)
    out.write(reinterpret_cast<const char*>(hdr), 18);
    out.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    return FlushCaptureStream(out);
}

int PersistPhotoCapture(const std::filesystem::path& root,
                        int first_id,
                        int width,
                        int height,
                        const std::vector<unsigned char>& pixels,
                        const luminumbra::game::PhotoSidecar& side) {
    std::size_t bytes = 0;
    if (!CapturePixelBytes(width, height, 3, bytes) || pixels.size() != bytes || first_id <= 0)
        return 0;
    const auto photos = root / "photos";
    const auto thumbnails = root / "data" / "ui" / "captures";
    std::error_code error;
    std::filesystem::create_directories(photos, error);
    if (error)
        return 0;
    std::filesystem::create_directories(thumbnails, error);
    if (error)
        return 0;
    // At most 1024 reservations per shutter; existing directories are never reused.
    // Incomplete reservations remain unpublished. No rollback deletes by pathname.
    for (int attempt = 0; attempt < 1024 && first_id <= std::numeric_limits<int>::max() - attempt;
         ++attempt) {
        const int id = first_id + attempt;
        const std::string stamp = "photo-" + std::to_string(id);
        const auto photo_dir = photos / stamp;
        if (!std::filesystem::create_directory(photo_dir, error)) {
            if (error)
                return 0;
            continue;
        }
        const auto thumbnail_dir = thumbnails / ("cap_" + std::to_string(id));
        if (!std::filesystem::create_directory(thumbnail_dir, error))
            return 0;
        auto saved_side = side;
        saved_side.stamp = stamp;
        auto metadata = nlohmann::json::parse(luminumbra::game::SerializePhotoSidecar(saved_side));
        metadata["capture_width"] = width;
        metadata["capture_height"] = height;
        const auto json = metadata.dump();
        FreshCaptureFile image(photo_dir / "image.ppm");
        FreshCaptureFile thumbnail(thumbnail_dir / "thumbnail.tga");
        FreshCaptureFile sidecar(photo_dir / "verdict.photo.json");
        // Publication is last, after every required close. Directory creation
        // has no stream whose close could fail after gallery visibility.
        const auto publish = [&] {
            return std::filesystem::create_directory(thumbnail_dir / "complete", error);
        };
        if (!WritePhotoCaptureFiles(
                image, thumbnail, sidecar, width, height, pixels, json, publish))
            return 0;
        return id;
    }
    return 0;
}

bool ReadCompletedPhotoCapture(const std::filesystem::path& root, int id, int& stars, bool legacy) {
    if (id <= 0)
        return false;
    const auto stamp = "photo-" + std::to_string(id);
    const auto photos = root / "photos";
    const auto thumbnails = root / "data" / "ui" / "captures";
    const auto photo_dir = photos / stamp;
    const auto thumbnail_dir = thumbnails / ("cap_" + std::to_string(id));
    try {
        // Historical flat captures have no publication marker. Validate all their
        // required bytes; new captures always require the final completion marker.
        if (!legacy) {
            const Filesystem::VerifiedFileInput complete(
                thumbnail_dir / "complete", Filesystem::VerifiedFileInput::Kind::Directory);
        }
        Filesystem::VerifiedFileInput image(legacy ? photos / (stamp + ".ppm")
                                                   : photo_dir / "image.ppm");
        Filesystem::VerifiedFileInput thumbnail(legacy ? thumbnails /
                                                             ("cap_" + std::to_string(id) + ".tga")
                                                       : thumbnail_dir / "thumbnail.tga");
        Filesystem::VerifiedFileInput sidecar(legacy ? photos / (stamp + ".photo.json")
                                                     : photo_dir / "verdict.photo.json");
        // A sidecar fits one 64 KiB block. Empty, partial and oversized metadata refuse.
        std::array<char, 65536> buffer{};
        if (image.size() == 0 || thumbnail.size() < 18 || sidecar.size() == 0 ||
            sidecar.size() > buffer.size())
            return false;
        const auto count = sidecar.read(buffer);
        if (count != sidecar.size())
            return false;
        const auto metadata = nlohmann::json::parse(buffer.data(), buffer.data() + count);
        const auto rating = metadata.find("stars");
        if (rating == metadata.end() || !rating->is_number_integer())
            return false;
        if (*rating < 0 || *rating > 5 || metadata.value("stamp", std::string{}) != stamp)
            return false;
        const auto image_count = image.read(buffer);
        const std::string_view header(buffer.data(), image_count);
        if (header.substr(0, 3) != "P6\n")
            return false;
        int width = 0;
        int height = 0;
        const auto parsed_width =
            std::from_chars(header.data() + 3, header.data() + header.size(), width);
        if (parsed_width.ec != std::errc{} || parsed_width.ptr == header.data() + header.size() ||
            *parsed_width.ptr != ' ')
            return false;
        const auto parsed_height =
            std::from_chars(parsed_width.ptr + 1, header.data() + header.size(), height);
        if (parsed_height.ec != std::errc{} ||
            std::string_view(
                parsed_height.ptr,
                static_cast<std::size_t>(header.data() + header.size() - parsed_height.ptr))
                    .substr(0, 5) != "\n255\n")
            return false;
        std::size_t bytes = 0;
        if (!CapturePixelBytes(width, height, 3, bytes))
            return false;
        const auto expected_header =
            "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
        if (image.size() != expected_header.size() + bytes ||
            header.substr(0, expected_header.size()) != expected_header)
            return false;
        if (!legacy) {
            const auto capture_width = metadata.find("capture_width");
            const auto capture_height = metadata.find("capture_height");
            if (capture_width == metadata.end() || capture_height == metadata.end() ||
                !capture_width->is_number_integer() || !capture_height->is_number_integer() ||
                *capture_width != width || *capture_height != height)
                return false;
        }
        if (thumbnail.read(buffer) < 18)
            return false;
        const auto byte = [&](std::size_t index) {
            return static_cast<unsigned char>(buffer[index]);
        };
        const int thumb_width = byte(12) + 256 * byte(13);
        const int thumb_height = byte(14) + 256 * byte(15);
        std::size_t thumb_bytes = 0;
        if (byte(0) != 0 || byte(1) != 0 || byte(2) != 2 || byte(16) != 24 || byte(17) != 0 ||
            thumb_width > 512 || thumb_height > 512 ||
            !CapturePixelBytes(thumb_width, thumb_height, 3, thumb_bytes) ||
            thumbnail.size() != 18 + thumb_bytes)
            return false;
        stars = rating->get<int>();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace Luminumbra::Rendering
