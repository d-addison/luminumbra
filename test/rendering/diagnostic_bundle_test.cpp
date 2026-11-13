#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"
#include "rendering/DiagnosticCapture.h"
#include "rendering/DiagnosticDrawTable.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;

DiagnosticCaptureOptions Options(std::initializer_list<std::string_view> arguments,
                                 std::optional<std::string_view> environment = std::nullopt) {
    return ResolveDiagnosticCaptureOptions({arguments.begin(), arguments.size()}, environment);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint32_t Word(std::string_view bytes, std::size_t offset) {
    std::uint32_t word = 0;
    for (unsigned int channel = 0; channel < 4; ++channel) {
        word |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + channel)))
                << (channel * 8U);
    }
    return word;
}

class FrameSource : public DiagnosticBundleSource {
public:
    std::array<DiagnosticId, 4> identities{};
    int reads = 0;
    int fail_read = 0;
    bool invalid_material = false;
    bool throw_read = false;
    bool read_aov(DiagnosticAov aov, std::span<float> samples) override {
        ++reads;
        if (throw_read) {
            throw std::runtime_error("read refused");
        }
        if (reads == fail_read) {
            return false;
        }
        const std::size_t channels =
            aov == DiagnosticAov::Albedo || aov == DiagnosticAov::Normal ? 3 : 1;
        for (std::size_t pixel = 0; pixel < 4; ++pixel) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                samples[pixel * channels + channel] = static_cast<float>(pixel * 10 + channel);
            }
        }
        if (invalid_material && aov == DiagnosticAov::Material) {
            samples.back() = 0.5f;
        }
        return true;
    }
    bool read_ids(std::span<DiagnosticId> pixels) override {
        ++reads;
        if (reads == fail_read) {
            return false;
        }
        std::copy(identities.begin(), identities.end(), pixels.begin());
        return true;
    }
};

class LimitedBuffer : public std::stringbuf {
public:
    std::size_t limit = std::numeric_limits<std::size_t>::max();

private:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        const auto accepted = (std::min)(static_cast<std::size_t>(count), limit);
        limit -= accepted;
        return std::stringbuf::xsputn(bytes, static_cast<std::streamsize>(accepted));
    }
};

class MemoryOutput : public DiagnosticBundleOutput {
public:
    enum class Failure {
        None,
        Reserve,
        Open,
        Write,
        Flush,
        Close,
        Commit,
        Exception
    };
    Failure failure = Failure::None;
    std::size_t fail_index = 0;
    int reservations = 0;
    int rollbacks = 0;
    bool published = false;
    std::vector<std::string> names;
    std::vector<std::string> payloads;
    bool reserve(std::span<const std::string> filenames) override {
        ++reservations;
        if (failure == Failure::Reserve) {
            return false;
        }
        names.assign(filenames.begin(), filenames.end());
        return true;
    }
    std::ostream* open(std::size_t index) override {
        current_ = index;
        if (index == fail_index && failure == Failure::Open) {
            return nullptr;
        }
        buffer_.str("");
        buffer_.limit = index == fail_index && failure == Failure::Write
                            ? 3U
                            : std::numeric_limits<std::size_t>::max();
        stream_.clear();
        return &stream_;
    }
    bool close() override {
        if (current_ == fail_index && failure == Failure::Exception) {
            throw std::runtime_error("close refused");
        }
        stream_.flush();
        if (!stream_ ||
            (current_ == fail_index && (failure == Failure::Flush || failure == Failure::Close))) {
            return false;
        }
        payloads.push_back(buffer_.str());
        return true;
    }
    bool commit() override {
        if (failure == Failure::Commit) {
            return false;
        }
        published = true;
        return true;
    }
    void rollback() noexcept override {
        ++rollbacks;
        payloads.clear();
        published = false;
    }

private:
    std::size_t current_ = 0;
    LimitedBuffer buffer_;
    std::ostream stream_{&buffer_};
};

class DiagnosticBundleTest : public testing::Test {
protected:
    void SetUp() override {
        directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("diagnostic-bundle-");
        for (std::uint32_t index = 0; index < 4; ++index) {
            ASSERT_TRUE(draws.submit({.mesh_path = "res/meshes/rock.lmesh",
                                      .shader_variant = "deferred",
                                      .chunk = {},
                                      .lod = index,
                                      .ownership = DiagnosticOwnership::Live,
                                      .pass = DiagnosticPass::GBuffer,
                                      .provenance = DiagnosticProvenance::Mesh},
                                     source.identities[index]));
        }
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    bool write(MemoryOutput& output) {
        return WriteDiagnosticBundle(options, frame, draws, source, output);
    }
    bool write_files() {
        return WriteDiagnosticBundle(
            directory / frame.capture_filename, options, frame, draws, source);
    }
    void create_completed_capture() {
        std::ofstream image(directory / frame.capture_filename, std::ios::binary);
        image << "existing capture";
        ASSERT_TRUE(image.good());
        std::ofstream manifest(directory / frame.manifest_filename, std::ios::binary);
        manifest << nlohmann::json{
            {"schema", 1},
            {"status", "complete"},
            {"resolution", {{"width", frame.extent.width}, {"height", frame.extent.height}}},
            {"camera", {{"view", frame.view}, {"projection", frame.projection}}}};
        ASSERT_TRUE(manifest.good());
    }
    std::filesystem::path directory;
    DiagnosticDrawTable draws;
    FrameSource source;
    DiagnosticCaptureOptions options{.enabled = true, .probe = std::array<std::uint32_t, 2>{1, 0}};
    DiagnosticBundleFrame frame{.extent = {2, 2},
                                .frame = 42,
                                .view = {},
                                .projection = {},
                                .reversed_z = true,
                                .identity_complete = true,
                                .staging_bytes = 64,
                                .capture_filename = "view.png",
                                .manifest_filename = "view.manifest.json"};
};

TEST(DiagnosticBundleOptions, ResolvesOptInAndCliPrecedence) {
    EXPECT_FALSE(Options({}).enabled);
    EXPECT_FALSE(Options({}, "0").enabled);
    EXPECT_TRUE(Options({}, "1").enabled);
    EXPECT_TRUE(Options({"--capture-diagnostics"}, "0").enabled);
    EXPECT_TRUE(Options({"--capture-diagnostics"}, "invalid").enabled);
    EXPECT_TRUE(Options({"--capture-diagnostics"}, "").enabled);
    EXPECT_EQ(Options({"--capture-diagnostics", "--capture-probe=0,0"}).probe,
              (std::array<std::uint32_t, 2>{0, 0}));
    EXPECT_EQ(Options({"--capture-probe", "4294967295,1"}, "1").probe,
              (std::array<std::uint32_t, 2>{4294967295U, 1}));
    EXPECT_FALSE(Options({"--unrelated"}).probe.has_value());
}

TEST(DiagnosticBundleOptions, RefusesEveryMalformedEnvironmentAndProbe) {
    for (const auto* value : {"", "00", "01", "true", "false", "2", "-1", " 1", "1 "}) {
        EXPECT_THROW(Options({}, value), std::invalid_argument) << value;
    }
    for (const auto* value : {"",
                              "0",
                              "0,",
                              ",0",
                              "-1,0",
                              "0,-1",
                              "+1,0",
                              "1,2,3",
                              "1.0,2",
                              "1, 2",
                              "4294967296,0",
                              "0,4294967296",
                              "0,1x"}) {
        const std::array<std::string_view, 2> arguments{"--capture-diagnostics", ""};
        const std::string probe = std::string("--capture-probe=") + value;
        auto invalid = arguments;
        invalid[1] = probe;
        EXPECT_THROW(ResolveDiagnosticCaptureOptions(invalid, std::nullopt), std::invalid_argument)
            << value;
    }
    EXPECT_THROW(Options({"--capture-probe=0,0"}), std::invalid_argument);
    EXPECT_THROW(Options({"--capture-diagnostics", "--capture-probe"}), std::invalid_argument);
    EXPECT_THROW(Options({"--capture-diagnostics=1"}), std::invalid_argument);
    EXPECT_THROW(Options({"--capture-diagnostics", "--capture-probe=0,0", "--capture-probe=1,1"}),
                 std::invalid_argument);
    EXPECT_THROW(Options({"--capture-probe=bad", "--capture-diagnostics"}), std::invalid_argument);
}

TEST_F(DiagnosticBundleTest, RuntimeAdmissionRefusesBeforeSaveRootOutput) {
    auto program = std::string("client");
    auto diagnostics = std::string("--capture-diagnostics");
    auto save_option = std::string("--save-root");
    auto save_root = (directory / "must-not-exist").string();
    std::array arguments{program.data(), diagnostics.data(), save_option.data(), save_root.data()};
    EXPECT_THROW(Luminumbra::Client::ScenarioHarness::ParseRuntimePathOptions(
                     static_cast<int>(arguments.size()), arguments.data()),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory / "must-not-exist"));
    diagnostics = "--capture-probe=broken";
    arguments[1] = diagnostics.data();
    EXPECT_THROW(Luminumbra::Client::ScenarioHarness::ParseRuntimePathOptions(
                     static_cast<int>(arguments.size()), arguments.data()),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory / "must-not-exist"));
}

TEST_F(DiagnosticBundleTest, StreamsTopFirstIdsAndSamePixelAovsAndProbe) {
    MemoryOutput output;
    ASSERT_TRUE(write(output));
    ASSERT_TRUE(output.published);
    ASSERT_EQ(output.payloads.size(), 8U);
    EXPECT_EQ(source.reads, 10);
    EXPECT_EQ(output.names,
              (std::vector<std::string>{"view.png.albedo.pfm",
                                        "view.png.normal.pfm",
                                        "view.png.depth.pfm",
                                        "view.png.material.pfm",
                                        "view.png.ids.bin",
                                        "view.png.draws.json",
                                        "view.png.probe-1-0.json",
                                        "view.png.diagnostics.json"}));
    const std::array<std::size_t, 4> top_first{2, 3, 0, 1};
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            EXPECT_EQ(Word(output.payloads[4], 16 + pixel * 16 + channel * 4),
                      source.identities[top_first[pixel]][channel]);
        }
    }
    for (std::size_t plane = 0; plane < 4; ++plane) {
        const std::size_t channels = plane < 2 ? 3 : 1;
        const auto header = output.payloads[plane].find("-1.0\n") + 5;
        for (std::size_t pixel = 0; pixel < 4; ++pixel) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                // PFM returns to bottom-first wire order after normalization.
                EXPECT_EQ(std::bit_cast<float>(Word(output.payloads[plane],
                                                    header + (pixel * channels + channel) * 4)),
                          static_cast<float>(pixel * 10 + channel));
            }
        }
    }
    const auto probe = nlohmann::json::parse(output.payloads[6]);
    EXPECT_EQ(probe.at("id"), source.identities[3]);
    EXPECT_EQ(probe.at("albedo_linear"), (std::array<float, 3>{30, 31, 32}));
    EXPECT_EQ(probe.at("normal_view"), (std::array<float, 3>{30, 31, 32}));
    EXPECT_EQ(probe.at("depth_hardware"), 30);
    EXPECT_EQ(probe.at("material"), 30);
    const auto metadata = nlohmann::json::parse(output.payloads.back());
    EXPECT_EQ(metadata.at("frame"), 42);
    EXPECT_EQ(metadata.at("capture_manifest"), "view.manifest.json");
    EXPECT_EQ(metadata.at("surface"), "deferred_only");
    EXPECT_TRUE(metadata.at("camera").at("reversed_z").get<bool>());
    EXPECT_TRUE(metadata.at("unsupported_producers").empty());
    EXPECT_EQ(metadata.at("files"), output.names);
}

TEST_F(DiagnosticBundleTest, RefusesInvalidFrameAndBudgetBeforeReadbackOrReservation) {
    const auto original = frame;
    for (int variant = 0; variant < 10; ++variant) {
        frame = original;
        switch (variant) {
            case 0:
                frame.extent.width = 0;
                break;
            case 1:
                frame.extent = {16384, 16384};
                break;
            case 2:
                frame.staging_bytes = std::numeric_limits<std::size_t>::max();
                break;
            case 3:
                frame.staging_bytes = 512U * 1024U * 1024U;
                break;
            case 4:
                frame.identity_complete = false;
                break;
            case 5:
                frame.capture_filename = "../escape.png";
                break;
            case 6:
                frame.manifest_filename = "C:escape";
                break;
            case 7:
                frame.view[0] = std::numeric_limits<float>::infinity();
                break;
            case 8:
                frame.extent.width = 1;
                break;
            case 9:
                frame.manifest_filename = "view.png.ids.bin";
                break;
        }
        MemoryOutput output;
        EXPECT_FALSE(write(output)) << variant;
        EXPECT_EQ(source.reads, 0) << variant;
        EXPECT_EQ(output.reservations, 0) << variant;
    }
}

TEST_F(DiagnosticBundleTest, ExactStagingBoundaryIsCheckedBeforeAnyImageStorage) {
    constexpr std::size_t overhead = 64U * 1024U * 1024U + 4096U;
    frame.staging_bytes = 512U * 1024U * 1024U - overhead - 4U * sizeof(DiagnosticId);
    MemoryOutput accepted;
    ASSERT_TRUE(write(accepted));
    ++frame.staging_bytes;
    source.reads = 0;
    MemoryOutput refused;
    EXPECT_FALSE(write(refused));
    EXPECT_EQ(source.reads, 0);
    EXPECT_EQ(refused.reservations, 0);
}

TEST_F(DiagnosticBundleTest, InvalidNamesRefuseBeforeReadbackAndBoundedUtf8NamesSerialize) {
    const auto original = frame;
    const std::array<std::string, 10> invalid{"",
                                              ".",
                                              "..",
                                              "../view.png",
                                              "dir\\view.png",
                                              "C:view.png",
                                              std::string("view\0.png", 9),
                                              std::string("view\x7f.png"),
                                              std::string(1025, 'x'),
                                              std::string(1, static_cast<char>(0xff))};
    for (const auto& name : invalid) {
        for (const bool manifest : {false, true}) {
            frame = original;
            if (manifest) {
                frame.manifest_filename = name;
            } else {
                frame.capture_filename = name;
            }
            MemoryOutput output;
            EXPECT_FALSE(write(output));
            EXPECT_EQ(source.reads, 0);
            EXPECT_EQ(output.reservations, 0);
        }
    }
    frame = original;
    frame.capture_filename = "vue-\xc3\xa9.png";
    MemoryOutput utf8;
    ASSERT_TRUE(write(utf8));
    EXPECT_EQ(utf8.names.front(), "vue-\xc3\xa9.png.albedo.pfm");
    frame.capture_filename.assign(1024, 'x');
    MemoryOutput bounded;
    EXPECT_TRUE(write(bounded));
}

TEST_F(DiagnosticBundleTest, InvalidMaterialsAndUnresolvedOrMismatchedIdsRefuseBeforeOutput) {
    const auto original = source.identities;
    for (int variant = 0; variant < 5; ++variant) {
        source.identities = original;
        source.invalid_material = variant == 0;
        switch (variant) {
            case 1:
                source.identities[0][0] = 99;
                break;
            case 2:
                source.identities[0][1] = 2;
                break;
            case 3:
                source.identities[0][3] = 0x10000;
                break;
            case 4:
                source.identities[0][2] = 255;
                break;
            default:
                break;
        }
        MemoryOutput output;
        EXPECT_FALSE(write(output)) << variant;
        EXPECT_EQ(output.reservations, 0) << variant;
    }
}

TEST_F(DiagnosticBundleTest, EveryReadAndIoFailureLeavesNoPublishedBundle) {
    for (int read = 1; read <= 10; ++read) {
        source.reads = 0;
        source.fail_read = read;
        MemoryOutput output;
        EXPECT_FALSE(write(output)) << read;
        EXPECT_FALSE(output.published) << read;
        EXPECT_TRUE(output.payloads.empty()) << read;
    }
    source.fail_read = 0;
    for (const auto failure : {MemoryOutput::Failure::Reserve,
                               MemoryOutput::Failure::Open,
                               MemoryOutput::Failure::Write,
                               MemoryOutput::Failure::Flush,
                               MemoryOutput::Failure::Close,
                               MemoryOutput::Failure::Commit,
                               MemoryOutput::Failure::Exception}) {
        for (std::size_t index = 0; index < 8; ++index) {
            MemoryOutput output;
            output.failure = failure;
            output.fail_index = index;
            EXPECT_FALSE(write(output)) << static_cast<int>(failure) << ':' << index;
            EXPECT_FALSE(output.published);
            EXPECT_TRUE(output.payloads.empty());
        }
    }
    source.throw_read = true;
    MemoryOutput output;
    EXPECT_FALSE(write(output));
    EXPECT_FALSE(output.published);
}

TEST_F(DiagnosticBundleTest, ForwardIdentitiesCannotDescribeTheDeferredSurface) {
    draws.clear();
    DiagnosticId forward{};
    ASSERT_TRUE(draws.submit({.mesh_path = "res/meshes/rock.lmesh",
                              .shader_variant = "forward",
                              .chunk = {},
                              .lod = 0,
                              .ownership = DiagnosticOwnership::Live,
                              .pass = DiagnosticPass::Forward,
                              .provenance = DiagnosticProvenance::Mesh},
                             forward));
    source.identities.fill(forward);
    MemoryOutput output;
    EXPECT_FALSE(write(output));
    EXPECT_EQ(output.reservations, 0);
}

TEST_F(DiagnosticBundleTest, ExplicitUnsupportedDeferredRecordsRefuseBeforeReadback) {
    DiagnosticId unsupported{};
    ASSERT_TRUE(draws.submit({.mesh_path = "",
                              .shader_variant = "deferred_producer",
                              .chunk = {},
                              .lod = 0,
                              .ownership = DiagnosticOwnership::Live,
                              .pass = DiagnosticPass::GBuffer,
                              .provenance = DiagnosticProvenance::Unsupported},
                             unsupported));
    MemoryOutput output;
    EXPECT_FALSE(write(output));
    EXPECT_EQ(source.reads, 0);
    EXPECT_EQ(output.reservations, 0);
}

TEST_F(DiagnosticBundleTest, FilesystemPublicationIsFreshAndCompletionIsLast) {
    create_completed_capture();
    ASSERT_TRUE(write_files());
    const auto complete = directory / "view.png.diagnostics.json";
    const auto metadata = ReadFile(complete);
    EXPECT_EQ(nlohmann::json::parse(metadata).at("status"), "complete");
    EXPECT_FALSE(std::filesystem::exists(directory / "view.png.diagnostics.json.pending"));
    EXPECT_FALSE(write_files());
    EXPECT_EQ(ReadFile(complete), metadata);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator()),
              10);
}

TEST_F(DiagnosticBundleTest, ExistingPayloadAndStagingRefuseWithoutChangingAnyFiles) {
    create_completed_capture();
    for (const auto* suffix : {".albedo.pfm",
                               ".normal.pfm",
                               ".depth.pfm",
                               ".material.pfm",
                               ".ids.bin",
                               ".draws.json",
                               ".probe-1-0.json",
                               ".diagnostics.json",
                               ".diagnostics.json.pending"}) {
        const auto existing = directory / (std::string("view.png") + suffix);
        {
            std::ofstream file(existing);
            file << "original";
        }
        EXPECT_FALSE(write_files()) << suffix;
        EXPECT_EQ(ReadFile(existing), "original") << suffix;
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                                std::filesystem::directory_iterator()),
                  3)
            << suffix;
        std::filesystem::remove(existing);
    }
}

TEST_F(DiagnosticBundleTest, FilesystemReadFailureRollsBackOnlyOwnedArtifacts) {
    create_completed_capture();
    source.fail_read = 8;
    const auto unrelated = directory / "keep.txt";
    {
        std::ofstream file(unrelated);
        file << "keep";
    }
    EXPECT_FALSE(write_files());
    EXPECT_FALSE(std::filesystem::exists(directory / "view.png.diagnostics.json"));
    EXPECT_EQ(ReadFile(unrelated), "keep");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator()),
              3);
}

TEST_F(DiagnosticBundleTest, MissingPartialOrMismatchedManifestRefusesBeforeReadback) {
    EXPECT_FALSE(write_files());
    EXPECT_EQ(source.reads, 0);
    create_completed_capture();
    const auto original = ReadFile(directory / frame.manifest_filename);
    for (int variant = 0; variant < 7; ++variant) {
        auto document = nlohmann::json::parse(original);
        switch (variant) {
            case 0:
                document["status"] = "incomplete";
                break;
            case 1:
                document["resolution"]["width"] = 3;
                break;
            case 2:
                document["camera"]["view"][0] = 1;
                break;
            case 3:
                document.erase("camera");
                break;
            case 4:
                document["schema"] = 2;
                break;
            default:
                break;
        }
        {
            std::ofstream manifest(directory / frame.manifest_filename, std::ios::binary);
            if (variant == 5) {
                manifest << "{partial";
            } else if (variant == 6) {
                manifest << std::string(1024U * 1024U + 1U, ' ');
            } else {
                manifest << document;
            }
        }
        EXPECT_FALSE(write_files()) << variant;
        EXPECT_EQ(source.reads, 0) << variant;
        EXPECT_FALSE(std::filesystem::exists(directory / "view.png.diagnostics.json"));
    }
    { std::ofstream image(directory / frame.capture_filename, std::ios::binary); }
    EXPECT_FALSE(write_files());
    EXPECT_EQ(source.reads, 0);
}

TEST_F(DiagnosticBundleTest, CaptureAndManifestWrongTypesOrLinksNeverAuthorizeReadback) {
    create_completed_capture();
    for (const auto& filename : {frame.capture_filename, frame.manifest_filename}) {
        const auto input = directory / filename;
        auto retained = input;
        retained += ".retained";
        std::filesystem::rename(input, retained);
        std::filesystem::create_directory(input);
        EXPECT_FALSE(write_files());
        EXPECT_EQ(source.reads, 0);
        std::filesystem::remove(input);
        std::error_code error;
        std::filesystem::create_symlink(retained, input, error);
        // Denied symlink creation also leaves a missing input and must refuse;
        // neither platform branch skips its admission assertions.
        EXPECT_FALSE(write_files());
        EXPECT_EQ(source.reads, 0);
        EXPECT_FALSE(std::filesystem::exists(directory / "view.png.diagnostics.json"));
        if (!error) {
            EXPECT_TRUE(std::filesystem::is_symlink(input));
            std::filesystem::remove(input);
        } else {
            EXPECT_FALSE(std::filesystem::exists(input));
        }
        std::filesystem::rename(retained, input);
    }
}

class ReplacingSource : public FrameSource {
public:
    std::filesystem::path payload;
    bool read_aov(DiagnosticAov aov, std::span<float> samples) override {
        if (reads == 6) {
            // Rename the closed payload away, preserving its original identity;
            // the newly created occupant must never be claimed by rollback.
            auto retired = payload;
            retired += ".retired";
            std::filesystem::rename(payload, retired);
            std::ofstream replacement(payload, std::ios::binary);
            replacement << "replacement";
            return false;
        }
        return FrameSource::read_aov(aov, samples);
    }
};

TEST_F(DiagnosticBundleTest, RollbackPreservesAReplacementInsteadOfRemovingByName) {
    create_completed_capture();
    ReplacingSource replacement;
    replacement.identities = source.identities;
    replacement.payload = directory / "view.png.albedo.pfm";
    EXPECT_FALSE(WriteDiagnosticBundle(
        directory / frame.capture_filename, options, frame, draws, replacement));
    EXPECT_EQ(ReadFile(replacement.payload), "replacement");
    EXPECT_TRUE(std::filesystem::exists(directory / "view.png.albedo.pfm.retired"));
    EXPECT_FALSE(std::filesystem::exists(directory / "view.png.diagnostics.json"));
}

TEST_F(DiagnosticBundleTest, UnsupportedCoverageUsesOnlyExplicitSubmissionRecords) {
    DiagnosticId unsupported{};
    ASSERT_TRUE(draws.submit({.mesh_path = "",
                              .shader_variant = "forward_contributor",
                              .chunk = {},
                              .lod = 0,
                              .ownership = DiagnosticOwnership::Live,
                              .pass = DiagnosticPass::Forward,
                              .provenance = DiagnosticProvenance::Unsupported},
                             unsupported));
    MemoryOutput output;
    ASSERT_TRUE(write(output));
    const auto metadata = nlohmann::json::parse(output.payloads.back());
    EXPECT_FALSE(metadata.at("coverage_complete").get<bool>());
    ASSERT_EQ(metadata.at("unsupported_producers").size(), 1U);
    EXPECT_EQ(metadata.at("unsupported_producers")[0].at("shader_variant"), "forward_contributor");
    EXPECT_EQ(metadata.at("unsupported_producers")[0].at("id"), unsupported[0]);
}

TEST_F(DiagnosticBundleTest, OffIsNoOpEvenWithInvalidInputsAndFailingTransports) {
    options.enabled = false;
    frame = {};
    source.throw_read = true;
    MemoryOutput output;
    output.failure = MemoryOutput::Failure::Exception;
    EXPECT_TRUE(write(output));
    EXPECT_EQ(source.reads, 0);
    EXPECT_EQ(output.reservations, 0);
    EXPECT_EQ(output.rollbacks, 0);
    EXPECT_TRUE(WriteDiagnosticBundle({}, options, frame, draws, source));
    EXPECT_TRUE(std::filesystem::is_empty(directory));
}

TEST_F(DiagnosticBundleTest, OptionalProbeAndBackgroundAndMissingDestination) {
    options.probe.reset();
    source.identities.fill({});
    draws.clear();
    MemoryOutput output;
    ASSERT_TRUE(write(output));
    EXPECT_EQ(output.payloads.size(), 7U);
    EXPECT_EQ(output.names.back(), "view.png.diagnostics.json");
    EXPECT_FALSE(
        WriteDiagnosticBundle(directory / "missing" / "view.png", options, frame, draws, source));
    EXPECT_FALSE(std::filesystem::exists(directory / "missing"));
    EXPECT_FALSE(WriteDiagnosticBundle(directory / "other.png", options, frame, draws, source));
    EXPECT_TRUE(std::filesystem::is_empty(directory));
}
} // namespace
