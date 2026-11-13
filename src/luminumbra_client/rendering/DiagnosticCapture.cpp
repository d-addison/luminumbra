#include "DiagnosticCapture.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ostream>

namespace Luminumbra::Rendering {
namespace {

constexpr std::size_t kMaxPixels = 16777216;
constexpr std::uint32_t kMaxDimension = 16384;

bool PixelCount(const DiagnosticExtent& extent, std::size_t& count) {
    if (extent.width == 0 || extent.height == 0 || extent.width > kMaxDimension ||
        extent.height > kMaxDimension || extent.width > kMaxPixels / extent.height) {
        return false;
    }
    count = static_cast<std::size_t>(extent.width) * extent.height;
    return true;
}

bool KnownOwnership(std::uint32_t value) {
    return value >= static_cast<std::uint32_t>(DiagnosticOwnership::Live) &&
           value <= static_cast<std::uint32_t>(DiagnosticOwnership::LadderFar);
}

bool KnownPass(std::uint32_t value) {
    return value >= static_cast<std::uint32_t>(DiagnosticPass::GBuffer) &&
           value <= static_cast<std::uint32_t>(DiagnosticPass::Sky);
}

// Batches little-endian words into a fixed 4 KiB buffer on all platforms.
class WordWriter {
public:
    explicit WordWriter(std::ostream& stream)
        : stream_(stream) {}

    bool word(std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            buffer_[used_++] = static_cast<char>((value >> shift) & 255U);
        }
        return used_ != buffer_.size() || flush();
    }

    bool flush() {
        stream_.write(buffer_.data(), static_cast<std::streamsize>(used_));
        used_ = 0;
        return static_cast<bool>(stream_);
    }

private:
    std::ostream& stream_;
    std::array<char, 4096> buffer_{};
    std::size_t used_ = 0;
};

unsigned int Channels(DiagnosticAov aov) {
    switch (aov) {
        case DiagnosticAov::Albedo:
        case DiagnosticAov::Normal:
            return 3;
        case DiagnosticAov::Depth:
        case DiagnosticAov::Material:
            return 1;
    }
    return 0;
}

bool ValidMaterial(float value) {
    return std::isfinite(value) && value >= 0 && value <= 255 && std::trunc(value) == value;
}

bool FiniteSample(const DiagnosticSample& sample) {
    for (float value : sample.albedo) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (float value : sample.normal) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return std::isfinite(sample.depth);
}

} // namespace

bool EncodeDiagnosticId(const DiagnosticIdentity& identity, DiagnosticId& output) {
    const auto ownership = static_cast<std::uint32_t>(identity.ownership);
    const auto pass = static_cast<std::uint32_t>(identity.pass);
    if (identity.draw == 0 || identity.mesh == 0 || identity.lod > 255 ||
        !KnownOwnership(ownership) || !KnownPass(pass)) {
        return false;
    }
    output = {identity.draw, identity.mesh, identity.lod, ownership | (pass << 8U)};
    return true;
}

bool ValidDiagnosticId(const DiagnosticId& id) {
    if (id == DiagnosticId{}) {
        return true;
    }
    return id[0] != 0 && id[1] != 0 && id[2] <= 255 && (id[3] & 0xffff0000U) == 0 &&
           KnownOwnership(id[3] & 255U) && KnownPass((id[3] >> 8U) & 255U);
}

bool WriteDiagnosticIds(std::ostream& stream,
                        const DiagnosticExtent& extent,
                        std::span<const DiagnosticId> pixels) {
    std::size_t count = 0;
    if (!PixelCount(extent, count) || pixels.size() != count) {
        return false;
    }
    for (const auto& pixel : pixels) {
        if (!ValidDiagnosticId(pixel)) {
            return false;
        }
    }
    try {
        stream.write("LVID", 4);
        WordWriter writer(stream);
        if (!writer.word(1) || !writer.word(extent.width) || !writer.word(extent.height)) {
            return false;
        }
        for (const auto& pixel : pixels) {
            for (const auto value : pixel) {
                if (!writer.word(value)) {
                    return false;
                }
            }
        }
        return writer.flush();
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

bool WriteDiagnosticAov(std::ostream& stream,
                        const DiagnosticExtent& extent,
                        DiagnosticAov aov,
                        std::span<const float> samples) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    std::size_t count = 0;
    const unsigned int channels = Channels(aov);
    if (!PixelCount(extent, count) || channels == 0 ||
        count > std::numeric_limits<std::size_t>::max() / channels ||
        samples.size() != count * channels) {
        return false;
    }
    if (aov == DiagnosticAov::Material) {
        for (float value : samples) {
            if (!ValidMaterial(value)) {
                return false;
            }
        }
    }
    try {
        // PFM rows are bottom-first; a negative scale declares little endian.
        const std::string header = std::string(channels == 3 ? "PF\n" : "Pf\n") +
                                   std::to_string(extent.width) + " " +
                                   std::to_string(extent.height) + "\n-1.0\n";
        stream.write(header.data(), static_cast<std::streamsize>(header.size()));
        WordWriter writer(stream);
        const std::size_t row_samples = static_cast<std::size_t>(extent.width) * channels;
        for (std::size_t row = extent.height; row > 0; --row) {
            const std::size_t begin = (row - 1) * row_samples;
            for (std::size_t column = 0; column < row_samples; ++column) {
                if (!writer.word(std::bit_cast<std::uint32_t>(samples[begin + column]))) {
                    return false;
                }
            }
        }
        return writer.flush();
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

std::string BuildDiagnosticProbe(const DiagnosticExtent& extent,
                                 std::uint32_t x,
                                 std::uint32_t y,
                                 const DiagnosticSample& sample) {
    std::size_t count = 0;
    if (!PixelCount(extent, count) || x >= extent.width || y >= extent.height ||
        !ValidDiagnosticId(sample.id) || sample.material > 255) {
        return {};
    }
    const nlohmann::json document = {
        {"schema", 1},
        {"origin", "top_left"},
        {"width", extent.width},
        {"height", extent.height},
        {"x", x},
        {"y", y},
        {"background", sample.id == DiagnosticId{}},
        {"id", sample.id},
        {"draw", sample.id[0]},
        {"mesh", sample.id[1]},
        {"lod", sample.id[2]},
        {"ownership", sample.id[3] & 255U},
        {"pass", (sample.id[3] >> 8U) & 255U},
        {"albedo_linear", sample.albedo},
        {"normal_view", sample.normal},
        {"depth_hardware", sample.depth},
        {"material", sample.material},
        {"non_finite", !FiniteSample(sample)},
    };
    return document.dump() + '\n';
}

} // namespace Luminumbra::Rendering
