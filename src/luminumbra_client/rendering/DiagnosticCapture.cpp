#include "DiagnosticCapture.h"
#include "DiagnosticDrawTable.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/CreatedWorldArtifacts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <utility>
#include <vector>

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

namespace Luminumbra::Rendering {
namespace {

bool ParseCoordinate(std::string_view text, std::uint32_t& coordinate) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), coordinate);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool BundleLeaf(std::string_view name) {
    if (name.empty() || name.size() > 1024 || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string_view::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 32 && byte != 127;
    });
}

bool BundleBudget(const DiagnosticBundleFrame& frame, std::size_t& pixels) {
    // One largest CPU plane (16 bytes/pixel), codec scratch and at most 64 MiB
    // for bounded table/coverage JSON, metadata and their serialization copies.
    constexpr std::size_t limit = 512U * 1024U * 1024U;
    constexpr std::size_t overhead = 64U * 1024U * 1024U + 4096U;
    if (!PixelCount(frame.extent, pixels) || frame.staging_bytes > limit - overhead) {
        return false;
    }
    return pixels <= (limit - overhead - frame.staging_bytes) / sizeof(DiagnosticId);
}

template<class Value>
void TopFirst(std::span<Value> values, const DiagnosticExtent& extent, std::size_t channels) {
    const auto row = static_cast<std::size_t>(extent.width) * channels;
    for (std::size_t y = 0; y < extent.height / 2; ++y) {
        std::swap_ranges(values.begin() + static_cast<std::ptrdiff_t>(y * row),
                         values.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         values.begin() +
                             static_cast<std::ptrdiff_t>((extent.height - 1 - y) * row));
    }
}

bool JoinedIds(std::span<const DiagnosticId> ids, const DiagnosticDrawTable& draws) {
    for (const auto& id : ids) {
        if (!ValidDiagnosticId(id)) {
            return false;
        }
        if (id[0] != 0) {
            const auto* joined = draws.resolve(id[0]);
            if (joined == nullptr || *joined != id ||
                (id[3] >> 8U) != static_cast<std::uint32_t>(DiagnosticPass::GBuffer)) {
                return false;
            }
        }
    }
    return true;
}

struct BundlePlan {
    std::size_t pixels = 0;
    std::size_t probe_index = 0;
    std::vector<std::string> filenames;
    std::string draws;
    std::string metadata;
};

bool PlanBundle(const DiagnosticCaptureOptions& options,
                const DiagnosticBundleFrame& frame,
                const DiagnosticDrawTable& draws,
                BundlePlan& plan) {
    if (!frame.identity_complete || !BundleBudget(frame, plan.pixels) ||
        !draws.complete_deferred_coverage() || !BundleLeaf(frame.capture_filename) ||
        !BundleLeaf(frame.manifest_filename) || frame.capture_filename == frame.manifest_filename ||
        !std::all_of(
            frame.view.begin(), frame.view.end(), [](float v) { return std::isfinite(v); }) ||
        !std::all_of(frame.projection.begin(), frame.projection.end(), [](float v) {
            return std::isfinite(v);
        })) {
        return false;
    }
    if (options.probe) {
        const auto& coordinates = options.probe.value();
        if (coordinates[0] >= frame.extent.width || coordinates[1] >= frame.extent.height) {
            return false;
        }
        plan.probe_index =
            static_cast<std::size_t>(coordinates[1]) * frame.extent.width + coordinates[0];
    }
    // Exactly eight bundle members at most; filenames and metadata are bounded.
    for (const auto* suffix :
         {".albedo.pfm", ".normal.pfm", ".depth.pfm", ".material.pfm", ".ids.bin", ".draws.json"}) {
        plan.filenames.push_back(frame.capture_filename + suffix);
    }
    if (options.probe) {
        const auto& coordinates = options.probe.value();
        plan.filenames.push_back(frame.capture_filename + ".probe-" +
                                 std::to_string(coordinates[0]) + "-" +
                                 std::to_string(coordinates[1]) + ".json");
    }
    plan.filenames.push_back(frame.capture_filename + ".diagnostics.json");
    if (std::find(plan.filenames.begin(), plan.filenames.end(), frame.manifest_filename) !=
        plan.filenames.end()) {
        return false;
    }
    plan.draws = draws.json();
    nlohmann::json metadata{{"schema", 1},
                            {"status", "complete"},
                            {"frame", frame.frame},
                            {"capture_manifest", frame.manifest_filename},
                            {"width", frame.extent.width},
                            {"height", frame.extent.height},
                            {"origin", "top_left"},
                            {"surface", "deferred_only"},
                            {"camera",
                             {{"view", frame.view},
                              {"projection", frame.projection},
                              {"matrix_order", "column_major"},
                              {"reversed_z", frame.reversed_z}}},
                            {"encodings",
                             {{"albedo", "linear_rgb_float32"},
                              {"normal", "signed_view_xyz_float32"},
                              {"depth", "hardware_depth_float32"},
                              {"material", "integral_0_255_float32"},
                              {"ids", "LVID_1_RGBA32UI_little_endian"}}},
                            {"files", plan.filenames},
                            {"identity_complete", frame.identity_complete},
                            {"coverage_complete", draws.complete_coverage()}};
    // Append the bounded, already validated record JSON without building another
    // per-draw DOM. No unordered iteration or inferred producer names.
    plan.metadata = metadata.dump();
    plan.metadata.reserve(plan.draws.size() + plan.metadata.size() + 64);
    plan.metadata.pop_back();
    plan.metadata += ",\"unsupported_producers\":";
    draws.append_unsupported_json(plan.metadata);
    plan.metadata += "}\n";
    return true;
}

bool AovPlane(DiagnosticAov aov,
              const DiagnosticBundleFrame& frame,
              const BundlePlan& plan,
              DiagnosticBundleSource& source,
              DiagnosticSample& sample,
              std::ostream* output) {
    const auto channels = Channels(aov);
    std::vector<float> values(plan.pixels * channels);
    if (!source.read_aov(aov, values)) {
        return false;
    }
    if (aov == DiagnosticAov::Material &&
        !std::all_of(values.begin(), values.end(), ValidMaterial)) {
        return false;
    }
    TopFirst<float>(values, frame.extent, channels);
    const auto index = plan.probe_index * channels;
    switch (aov) {
        case DiagnosticAov::Albedo:
            std::copy_n(
                values.begin() + static_cast<std::ptrdiff_t>(index), 3, sample.albedo.begin());
            break;
        case DiagnosticAov::Normal:
            std::copy_n(
                values.begin() + static_cast<std::ptrdiff_t>(index), 3, sample.normal.begin());
            break;
        case DiagnosticAov::Depth:
            sample.depth = values[index];
            break;
        case DiagnosticAov::Material:
            sample.material = static_cast<std::uint32_t>(values[index]);
            break;
    }
    return output == nullptr || WriteDiagnosticAov(*output, frame.extent, aov, values);
}

bool IdPlane(const DiagnosticBundleFrame& frame,
             const BundlePlan& plan,
             const DiagnosticDrawTable& draws,
             DiagnosticBundleSource& source,
             DiagnosticSample& sample,
             std::ostream* output) {
    std::vector<DiagnosticId> values(plan.pixels);
    if (!source.read_ids(values) || !JoinedIds(values, draws)) {
        return false;
    }
    TopFirst<DiagnosticId>(values, frame.extent, 1);
    sample.id = values[plan.probe_index];
    return output == nullptr || WriteDiagnosticIds(*output, frame.extent, values);
}

// The streambuf forwards each codec's bounded block to the creating FILE.
class BundleFileBuffer : public std::streambuf {
public:
    explicit BundleFileBuffer(std::FILE* file)
        : file_(file) {}

private:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        if (count <= 0) {
            return 0;
        }
        return static_cast<std::streamsize>(
            std::fwrite(bytes, 1, static_cast<std::size_t>(count), file_));
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        return std::fputc(traits_type::to_char_type(character), file_) == EOF ? traits_type::eof()
                                                                              : character;
    }
    int sync() override {
        return std::fflush(file_);
    }
    std::FILE* file_;
};

class BundleFiles : public DiagnosticBundleOutput {
public:
    explicit BundleFiles(const std::filesystem::path& directory)
        : directory_(directory) {}
    ~BundleFiles() override {
        rollback();
    }
    bool reserve(std::span<const std::string> filenames) override {
        journal_.bind_existing_root(directory_);
        directory_ = std::filesystem::canonical(directory_);
        for (const auto& filename : filenames) {
            paths_.push_back(directory_ / Filesystem::PathFromUtf8(filename));
            journal_.reserve_file(paths_.back());
        }
        completion_ = paths_.back();
        paths_.back() += ".pending";
        journal_.reserve_file(paths_.back());
        return true;
    }
    std::ostream* open(std::size_t index) override {
        if (file_ != nullptr || index >= paths_.size()) {
            return nullptr;
        }
        const auto& path = paths_[index];
#if defined(_WIN32)
        const auto native = ::CreateFileW(path.c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
        if (native == INVALID_HANDLE_VALUE) {
            return nullptr;
        }
        try {
            journal_.file_created(path, native);
        } catch (...) {
            ::CloseHandle(native);
            throw;
        }
        const auto descriptor =
            ::_open_osfhandle(reinterpret_cast<intptr_t>(native), _O_BINARY | _O_WRONLY);
        if (descriptor < 0) {
            ::CloseHandle(native);
            return nullptr;
        }
        file_ = ::_fdopen(descriptor, "wb");
        if (file_ == nullptr) {
            ::_close(descriptor);
            return nullptr;
        }
#else
        file_ = std::fopen(path.c_str(), "wbx");
        if (file_ == nullptr) {
            return nullptr;
        }
        journal_.file_created(path, ::fileno(file_));
#endif
        buffer_ = std::make_unique<BundleFileBuffer>(file_);
        stream_ = std::make_unique<std::ostream>(buffer_.get());
        return stream_.get();
    }
    bool close() override {
        if (file_ == nullptr) {
            return false;
        }
        const bool good = stream_ != nullptr && static_cast<bool>(*stream_);
        const bool flushed = std::fflush(file_) == 0;
        const bool closed = std::fclose(std::exchange(file_, nullptr)) == 0;
        stream_.reset();
        buffer_.reset();
        return good && flushed && closed;
    }
    bool commit() override {
        journal_.publish_file(paths_.back(), completion_);
        journal_.release();
        return true;
    }
    void rollback() noexcept override {
        if (file_ != nullptr) {
            close();
        }
        // The shared journal rechecks native identity at removal on both POSIX
        // and Windows, retaining any changed or unowned entry on refusal.
        try {
            journal_.rollback();
        } catch (...) {
            // A cleanup refusal retains unpublished artifacts for inspection.
        }
    }

private:
    std::filesystem::path directory_;
    Persistence::CreatedWorldArtifacts journal_;
    std::vector<std::filesystem::path> paths_;
    std::filesystem::path completion_;
    std::FILE* file_ = nullptr;
    std::unique_ptr<BundleFileBuffer> buffer_;
    std::unique_ptr<std::ostream> stream_;
};

bool BundlePayloads(const DiagnosticCaptureOptions& options,
                    const DiagnosticBundleFrame& frame,
                    const DiagnosticDrawTable& draws,
                    DiagnosticBundleSource& source,
                    DiagnosticBundleOutput& output) {
    BundlePlan plan;
    if (!PlanBundle(options, frame, draws, plan)) {
        return false;
    }
    DiagnosticSample sample;
    constexpr std::array planes{DiagnosticAov::Albedo,
                                DiagnosticAov::Normal,
                                DiagnosticAov::Depth,
                                DiagnosticAov::Material};
    // Preflight every source plane before reserving a destination. The immutable
    // source is read twice, never retained/copied as a complete multi-plane frame.
    for (const auto aov : planes) {
        if (!AovPlane(aov, frame, plan, source, sample, nullptr)) {
            return false;
        }
    }
    if (!IdPlane(frame, plan, draws, source, sample, nullptr) || !output.reserve(plan.filenames)) {
        return false;
    }
    for (std::size_t index = 0; index < planes.size(); ++index) {
        auto* stream = output.open(index);
        if (stream == nullptr || !AovPlane(planes[index], frame, plan, source, sample, stream) ||
            !output.close()) {
            return false;
        }
    }
    auto* ids = output.open(4);
    if (ids == nullptr || !IdPlane(frame, plan, draws, source, sample, ids) || !output.close()) {
        return false;
    }
    auto* table = output.open(5);
    if (table == nullptr) {
        return false;
    }
    *table << plan.draws;
    if (!*table || !output.close()) {
        return false;
    }
    std::size_t index = 6;
    if (options.probe) {
        const auto& coordinates = options.probe.value();
        const auto probe =
            BuildDiagnosticProbe(frame.extent, coordinates[0], coordinates[1], sample);
        auto* stream = output.open(index++);
        if (probe.empty() || stream == nullptr) {
            return false;
        }
        *stream << probe;
        if (!*stream || !output.close()) {
            return false;
        }
    }
    auto* completion = output.open(index);
    if (completion == nullptr) {
        return false;
    }
    *completion << plan.metadata;
    return static_cast<bool>(*completion) && output.close() && output.commit();
}

bool CompletedCapture(const std::filesystem::path& capture, const DiagnosticBundleFrame& frame) {
    auto manifest = capture;
    manifest.replace_extension(".manifest.json");
    if (Filesystem::GenericUtf8PathString(manifest.filename()) != frame.manifest_filename) {
        return false;
    }
    const Filesystem::VerifiedFileInput parent(
        capture.has_parent_path() ? capture.parent_path() : std::filesystem::path("."),
        Filesystem::VerifiedFileInput::Kind::Directory);
    const Filesystem::VerifiedFileInput image(capture);
    Filesystem::VerifiedFileInput input(manifest);
    if (image.size() == 0 ||
        !Filesystem::PathSpellingEqual(image.resolved_path().parent_path(),
                                       parent.resolved_path()) ||
        !Filesystem::PathSpellingEqual(input.resolved_path().parent_path(),
                                       parent.resolved_path())) {
        return false;
    }
    // Existing manifests contain bounded camera/settings metadata, not images.
    // Open once, inspect and read that same regular-file handle; never a FIFO.
    constexpr std::uint64_t max_manifest = 1024U * 1024U;
    const auto bytes = input.size();
    if (bytes == 0 || bytes > max_manifest) {
        return false;
    }
    std::string text(static_cast<std::size_t>(bytes), '\0');
    std::array<char, 65536> buffer{};
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = input.read(buffer);
        if (count == 0 || count > text.size() - offset) {
            return false;
        }
        std::copy_n(buffer.data(), count, text.data() + offset);
        offset += count;
    }
    if (input.read(buffer) != 0) {
        return false;
    }
    const auto document = nlohmann::json::parse(text);
    return document.at("schema") == 1 && document.at("status") == "complete" &&
           document.at("resolution").at("width") == frame.extent.width &&
           document.at("resolution").at("height") == frame.extent.height &&
           document.at("camera").at("view") == frame.view &&
           document.at("camera").at("projection") == frame.projection;
}

template<class Arguments>
DiagnosticCaptureOptions ResolveOptions(const Arguments& arguments,
                                        std::optional<std::string_view> environment) {
    DiagnosticCaptureOptions options;
    bool explicit_diagnostics = false;
    std::optional<std::string_view> probe;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--capture-diagnostics") {
            explicit_diagnostics = true;
        } else if (argument.starts_with("--capture-diagnostics=")) {
            throw std::invalid_argument("--capture-diagnostics takes no value");
        } else if (argument == "--capture-probe" || argument.starts_with("--capture-probe=")) {
            if (probe) {
                throw std::invalid_argument("--capture-probe may only be supplied once");
            }
            if (argument == "--capture-probe") {
                if (++index == arguments.size()) {
                    throw std::invalid_argument("--capture-probe requires X,Y");
                }
                probe = arguments[index];
            } else {
                probe = argument.substr(std::string_view("--capture-probe=").size());
            }
        }
    }
    if (!explicit_diagnostics && environment && environment.value() != "0" &&
        environment.value() != "1") {
        throw std::invalid_argument("LUMIN_CAPTURE_DIAGNOSTICS requires 0 or 1");
    }
    options.enabled = explicit_diagnostics || (environment && environment.value() == "1");
    if (probe) {
        const auto text = probe.value();
        const auto comma = text.find(',');
        std::array<std::uint32_t, 2> coordinates{};
        if (!options.enabled || comma == std::string_view::npos ||
            !ParseCoordinate(text.substr(0, comma), coordinates[0]) ||
            !ParseCoordinate(text.substr(comma + 1), coordinates[1])) {
            throw std::invalid_argument("--capture-probe requires diagnostics and integer X,Y");
        }
        options.probe = coordinates;
    }
    return options;
}

} // namespace

DiagnosticCaptureOptions
ResolveDiagnosticCaptureOptions(std::span<const std::string_view> arguments,
                                std::optional<std::string_view> environment) {
    return ResolveOptions(arguments, environment);
}

DiagnosticCaptureOptions
ResolveDiagnosticCaptureOptions(std::span<char* const> arguments,
                                std::optional<std::string_view> environment) {
    return ResolveOptions(arguments, environment);
}

bool WriteDiagnosticBundle(const DiagnosticCaptureOptions& options,
                           const DiagnosticBundleFrame& frame,
                           const DiagnosticDrawTable& draws,
                           DiagnosticBundleSource& source,
                           DiagnosticBundleOutput& output) {
    if (!options.enabled) {
        return true;
    }
    try {
        if (BundlePayloads(options, frame, draws, source, output)) {
            return true;
        }
    } catch (const std::exception&) {
        // Serialization and transport exceptions follow the same ownership path.
    }
    output.rollback();
    return false;
}

bool WriteDiagnosticBundle(const std::filesystem::path& capture,
                           const DiagnosticCaptureOptions& options,
                           const DiagnosticBundleFrame& frame,
                           const DiagnosticDrawTable& draws,
                           DiagnosticBundleSource& source) {
    if (!options.enabled) {
        return true;
    }
    try {
        std::size_t pixels = 0;
        if (!BundleBudget(frame, pixels) || !BundleLeaf(frame.capture_filename) ||
            !BundleLeaf(frame.manifest_filename) ||
            Filesystem::GenericUtf8PathString(capture.filename()) != frame.capture_filename ||
            !CompletedCapture(capture, frame)) {
            return false;
        }
        BundleFiles output(capture.has_parent_path() ? capture.parent_path()
                                                     : std::filesystem::path("."));
        return WriteDiagnosticBundle(options, frame, draws, source, output);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace Luminumbra::Rendering
