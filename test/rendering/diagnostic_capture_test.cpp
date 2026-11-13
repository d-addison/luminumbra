#include "rendering/DiagnosticCapture.h"
#include "rendering/DiagnosticDrawTable.h"
#include "rendering/DiagnosticTargets.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;

std::uint32_t Word(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(offset + index)))
                 << (index * 8U);
    }
    return value;
}

DiagnosticSample Sample() {
    return {.id = {0x12345678U, 0xfedcba98U, 2, 0x103},
            .albedo = {0.25f, 0.5f, 1.0f},
            .normal = {-1.0f, 0.0f, 1.0f},
            .depth = 0.75f,
            .material = 17};
}

TEST(DiagnosticCapture, IdentityEncodingPreservesFullIdsAndAllOwnersAndPasses) {
    for (const auto owner : {DiagnosticOwnership::Live,
                             DiagnosticOwnership::LegacyFar,
                             DiagnosticOwnership::LadderFar}) {
        for (const auto pass : {DiagnosticPass::GBuffer,
                                DiagnosticPass::Forward,
                                DiagnosticPass::Shadow,
                                DiagnosticPass::Sky}) {
            for (const std::uint32_t lod : {0U, 255U}) {
                DiagnosticId id{};
                ASSERT_TRUE(EncodeDiagnosticId({.draw = 0xffffffffU,
                                                .mesh = 0xfffffffeU,
                                                .lod = lod,
                                                .ownership = owner,
                                                .pass = pass},
                                               id));
                EXPECT_EQ(id[0], 0xffffffffU);
                EXPECT_EQ(id[1], 0xfffffffeU);
                EXPECT_EQ(id[2], lod);
                EXPECT_EQ(id[3],
                          static_cast<std::uint32_t>(owner) |
                              (static_cast<std::uint32_t>(pass) << 8U));
                EXPECT_TRUE(ValidDiagnosticId(id));
            }
        }
    }
    EXPECT_TRUE(ValidDiagnosticId({}));
}

TEST(DiagnosticCapture, InvalidIdentityDoesNotReplaceOutput) {
    const DiagnosticIdentity good{.draw = 1,
                                  .mesh = 2,
                                  .lod = 0,
                                  .ownership = DiagnosticOwnership::Live,
                                  .pass = DiagnosticPass::GBuffer};
    std::array<DiagnosticIdentity, 7> invalid{};
    invalid.fill(good);
    invalid[0].draw = 0;
    invalid[1].mesh = 0;
    invalid[2].lod = 256;
    invalid[3].ownership = static_cast<DiagnosticOwnership>(0);
    invalid[4].ownership = static_cast<DiagnosticOwnership>(4);
    invalid[5].pass = static_cast<DiagnosticPass>(0);
    invalid[6].pass = static_cast<DiagnosticPass>(5);
    for (const auto& identity : invalid) {
        DiagnosticId output{9, 8, 7, 6};
        EXPECT_FALSE(EncodeDiagnosticId(identity, output));
        EXPECT_EQ(output, (DiagnosticId{9, 8, 7, 6}));
    }
    for (const auto& id : std::array<DiagnosticId, 8>{{{0, 1, 0, 0x101},
                                                       {1, 0, 0, 0x101},
                                                       {1, 1, 256, 0x101},
                                                       {1, 1, 0, 0x100},
                                                       {1, 1, 0, 0x104},
                                                       {1, 1, 0, 1},
                                                       {1, 1, 0, 0x501},
                                                       {1, 1, 0, 0x10101}}}) {
        EXPECT_FALSE(ValidDiagnosticId(id));
    }
}

TEST(DiagnosticCapture, IdImageHasVersionedLittleEndianHeaderAndTopFirstRows) {
    const std::array<DiagnosticId, 4> pixels{Sample().id, {}, {1, 2, 0, 0x101}, {3, 4, 1, 0x102}};
    std::ostringstream stream;
    ASSERT_TRUE(WriteDiagnosticIds(stream, {2, 2}, pixels));
    const auto bytes = stream.str();
    ASSERT_EQ(bytes.size(), 16U + 16U * pixels.size());
    EXPECT_EQ(bytes.substr(0, 4), "LVID");
    EXPECT_EQ(Word(bytes, 4), 1U);
    EXPECT_EQ(Word(bytes, 8), 2U);
    EXPECT_EQ(Word(bytes, 12), 2U);
    for (std::size_t pixel = 0; pixel < pixels.size(); ++pixel) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            EXPECT_EQ(Word(bytes, 16 + pixel * 16 + channel * 4), pixels[pixel][channel]);
        }
    }
}

TEST(DiagnosticCapture, AovImagesPreserveFloatBitsAndUsePfmBottomFirstRows) {
    for (const auto aov : {DiagnosticAov::Albedo, DiagnosticAov::Normal}) {
        const std::array<float, 6> samples{0.25f, -1.0f, 2.0f, 0.5f, 0.0f, 1.0f};
        std::ostringstream stream;
        ASSERT_TRUE(WriteDiagnosticAov(stream, {1, 2}, aov, samples));
        const auto bytes = stream.str();
        const std::string header = "PF\n1 2\n-1.0\n";
        EXPECT_EQ(bytes.substr(0, header.size()), header);
        ASSERT_EQ(bytes.size(), header.size() + 24);
        for (std::size_t index = 0; index < samples.size(); ++index) {
            EXPECT_EQ(Word(bytes, header.size() + index * 4),
                      std::bit_cast<std::uint32_t>(samples[(index + 3) % 6]));
        }
    }
    for (const auto aov : {DiagnosticAov::Depth, DiagnosticAov::Material}) {
        const std::array<float, 2> samples{0.0f, 255.0f};
        std::ostringstream stream;
        ASSERT_TRUE(WriteDiagnosticAov(stream, {1, 2}, aov, samples));
        const auto bytes = stream.str();
        const std::string header = "Pf\n1 2\n-1.0\n";
        EXPECT_EQ(bytes.substr(0, header.size()), header);
        ASSERT_EQ(bytes.size(), header.size() + 8);
        EXPECT_EQ(Word(bytes, header.size()), std::bit_cast<std::uint32_t>(255.0f));
        EXPECT_EQ(Word(bytes, header.size() + 4), 0U);
    }
}

TEST(DiagnosticCapture, RefusesDimensionsExactSizesUnknownKindsAndLateInvalidDataBeforeWriting) {
    const std::array<DiagnosticId, 1> ids{Sample().id};
    const std::array<float, 1> scalar{0};
    for (const auto& extent : std::array<DiagnosticExtent, 7>{{{0, 1},
                                                               {1, 0},
                                                               {16385, 1},
                                                               {1, 16385},
                                                               {16384, 16384},
                                                               {0xffffffffU, 0xffffffffU},
                                                               {4096, 4097}}}) {
        std::ostringstream stream;
        EXPECT_FALSE(WriteDiagnosticIds(stream, extent, ids));
        EXPECT_FALSE(WriteDiagnosticAov(stream, extent, DiagnosticAov::Depth, scalar));
        EXPECT_TRUE(BuildDiagnosticProbe(extent, 0, 0, Sample()).empty());
        EXPECT_TRUE(stream.str().empty());
    }
    for (const std::size_t size : {0U, 1U, 3U, 5U}) {
        std::ostringstream stream;
        EXPECT_FALSE(WriteDiagnosticIds(stream, {2, 2}, std::vector<DiagnosticId>(size)));
        EXPECT_FALSE(
            WriteDiagnosticAov(stream, {2, 2}, DiagnosticAov::Depth, std::vector<float>(size)));
        EXPECT_FALSE(
            WriteDiagnosticAov(stream, {2, 2}, DiagnosticAov::Normal, std::vector<float>(size)));
        EXPECT_TRUE(stream.str().empty());
    }
    std::ostringstream stream;
    EXPECT_FALSE(WriteDiagnosticAov(stream, {1, 1}, static_cast<DiagnosticAov>(99), scalar));
    const std::array<DiagnosticId, 2> late_invalid{Sample().id, {1, 0, 0, 0x101}};
    EXPECT_FALSE(WriteDiagnosticIds(stream, {2, 1}, late_invalid));
    for (float value : {-1.0f,
                        256.0f,
                        0.5f,
                        std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        const std::array<float, 2> material{1.0f, value};
        EXPECT_FALSE(WriteDiagnosticAov(stream, {2, 1}, DiagnosticAov::Material, material));
    }
    EXPECT_TRUE(stream.str().empty());
}

TEST(DiagnosticCapture, ProbeCoordinatesBackgroundAndNonfiniteChannelsAreExplicit) {
    auto sample = Sample();
    EXPECT_FALSE(BuildDiagnosticProbe({16384, 1024}, 16383, 1023, sample).empty());
    EXPECT_FALSE(BuildDiagnosticProbe({1, 16384}, 0, 16383, sample).empty());
    const auto first = BuildDiagnosticProbe({2, 2}, 1, 0, sample);
    EXPECT_EQ(first, BuildDiagnosticProbe({2, 2}, 1, 0, sample));
    const auto document = nlohmann::json::parse(first);
    EXPECT_EQ(document.at("schema"), 1);
    EXPECT_EQ(document.at("origin"), "top_left");
    EXPECT_EQ(document.at("width"), 2);
    EXPECT_EQ(document.at("height"), 2);
    EXPECT_EQ(document.at("x"), 1);
    EXPECT_EQ(document.at("y"), 0);
    EXPECT_EQ(document.at("id"), nlohmann::json(sample.id));
    EXPECT_EQ(document.at("draw"), sample.id[0]);
    EXPECT_EQ(document.at("mesh"), sample.id[1]);
    EXPECT_EQ(document.at("lod"), 2);
    EXPECT_EQ(document.at("ownership"), 3);
    EXPECT_EQ(document.at("pass"), 1);
    EXPECT_EQ(document.at("material"), 17);
    EXPECT_EQ(document.at("albedo_linear"), nlohmann::json(sample.albedo));
    EXPECT_EQ(document.at("normal_view"), nlohmann::json(sample.normal));
    EXPECT_EQ(document.at("depth_hardware"), 0.75);
    EXPECT_EQ(document.at("non_finite"), false);
    EXPECT_EQ(document.at("background"), false);
    for (const auto& coordinate :
         std::array<std::array<std::uint32_t, 2>, 3>{{{2, 0}, {0, 2}, {0xffffffffU, 0}}}) {
        EXPECT_TRUE(BuildDiagnosticProbe({2, 2}, coordinate[0], coordinate[1], sample).empty());
    }
    sample.material = 256;
    EXPECT_TRUE(BuildDiagnosticProbe({2, 2}, 0, 0, sample).empty());
    sample.material = 0;
    sample.id = {0, 1, 0, 0x101};
    EXPECT_TRUE(BuildDiagnosticProbe({2, 2}, 0, 0, sample).empty());
    sample.id = {};
    EXPECT_EQ(nlohmann::json::parse(BuildDiagnosticProbe({1, 1}, 0, 0, sample)).at("background"),
              true);
    for (std::size_t channel = 0; channel < 7; ++channel) {
        sample = Sample();
        if (channel < 3) {
            sample.albedo[channel] = std::numeric_limits<float>::quiet_NaN();
        } else if (channel < 6) {
            sample.normal[channel - 3] = -std::numeric_limits<float>::infinity();
        } else {
            sample.depth = std::numeric_limits<float>::infinity();
        }
        const auto nonfinite = nlohmann::json::parse(BuildDiagnosticProbe({1, 1}, 0, 0, sample));
        EXPECT_EQ(nonfinite.at("non_finite"), true);
        if (channel < 3) {
            EXPECT_TRUE(nonfinite.at("albedo_linear").at(channel).is_null());
        } else if (channel < 6) {
            EXPECT_TRUE(nonfinite.at("normal_view").at(channel - 3).is_null());
        } else {
            EXPECT_TRUE(nonfinite.at("depth_hardware").is_null());
        }
    }
}

TEST(DiagnosticCapture, NonfiniteAovSamplesSurviveForOfflineChecks) {
    const std::array<float, 3> samples{std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::infinity(),
                                       -std::numeric_limits<float>::infinity()};
    for (const auto aov : {DiagnosticAov::Albedo, DiagnosticAov::Normal, DiagnosticAov::Depth}) {
        std::ostringstream stream;
        const DiagnosticExtent extent =
            aov == DiagnosticAov::Depth ? DiagnosticExtent{3, 1} : DiagnosticExtent{1, 1};
        ASSERT_TRUE(WriteDiagnosticAov(stream, extent, aov, samples));
        const auto bytes = stream.str();
        for (std::size_t index = 0; index < 3; ++index) {
            EXPECT_EQ(Word(bytes, bytes.size() - 12 + index * 4),
                      std::bit_cast<std::uint32_t>(samples[index]));
        }
    }
}

class LimitedBuffer : public std::streambuf {
public:
    explicit LimitedBuffer(std::streamsize limit)
        : remaining_(limit) {}

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        const auto accepted = std::min(count, remaining_);
        remaining_ -= accepted;
        return accepted;
    }

private:
    std::streamsize remaining_;
};

TEST(DiagnosticCapture, BatchedWritesAndPartialOrThrowingStreamsReportFailure) {
    const std::vector<DiagnosticId> pixels(257, Sample().id);
    const std::vector<float> samples(1025, 0.5f);
    std::ostringstream ids;
    ASSERT_TRUE(WriteDiagnosticIds(ids, {257, 1}, pixels));
    EXPECT_EQ(ids.str().size(), 16U + 257U * 16U);
    EXPECT_EQ(Word(ids.str(), 16 + 256 * 16), Sample().id[0]);
    std::ostringstream depth;
    ASSERT_TRUE(WriteDiagnosticAov(depth, {1025, 1}, DiagnosticAov::Depth, samples));
    EXPECT_EQ(Word(depth.str(), depth.str().size() - 4), std::bit_cast<std::uint32_t>(0.5f));
    for (const bool throwing : {false, true}) {
        for (const std::streamsize limit : {0, 4, 100, 4112}) {
            LimitedBuffer ids_buffer(limit);
            std::ostream id_stream(&ids_buffer);
            LimitedBuffer aov_buffer(limit);
            std::ostream aov_stream(&aov_buffer);
            if (throwing) {
                id_stream.exceptions(std::ios::badbit | std::ios::failbit);
                aov_stream.exceptions(std::ios::badbit | std::ios::failbit);
            }
            EXPECT_FALSE(WriteDiagnosticIds(id_stream, {257, 1}, pixels));
            EXPECT_FALSE(WriteDiagnosticAov(aov_stream, {1025, 1}, DiagnosticAov::Depth, samples));
        }
    }
}

} // namespace

namespace {

DiagnosticSubmission Submission() {
    return {.mesh_path = "res/meshes/terrain.mesh",
            .shader_variant = "gbuffer/ladder",
            .chunk = {-2, 0, 7},
            .lod = 3,
            .ownership = DiagnosticOwnership::LadderFar,
            .pass = DiagnosticPass::GBuffer,
            .provenance = DiagnosticProvenance::Sdf};
}

TEST(DiagnosticDrawTable, SubmissionOrderSharedMeshesExactMetadataAndReset) {
    DiagnosticDrawTable table;
    DiagnosticId first{};
    ASSERT_TRUE(table.submit(Submission(), first));
    EXPECT_EQ(first, (DiagnosticId{1, 1, 3, 0x103}));
    auto submission = Submission();
    submission.lod = 0;
    submission.chunk = {4, 5, 6};
    DiagnosticId second{};
    ASSERT_TRUE(table.submit(submission, second));
    EXPECT_EQ(second, (DiagnosticId{2, 1, 0, 0x103}));
    submission.mesh_path = "res/meshes/other.mesh";
    ASSERT_TRUE(table.submit(submission, second));
    EXPECT_EQ(second[0], 3U);
    EXPECT_EQ(second[1], 2U);
    const auto bytes = table.json();
    EXPECT_EQ(bytes, table.json());
    const auto doc = nlohmann::json::parse(bytes);
    ASSERT_EQ(doc.at("meshes").size(), 2U);
    ASSERT_EQ(doc.at("draws").size(), 3U);
    EXPECT_EQ(doc.at("meshes")[0].at("path"), Submission().mesh_path);
    EXPECT_EQ(doc.at("meshes")[0].at("provenance"), 2);
    EXPECT_EQ(doc.at("draws")[0].at("chunk"), nlohmann::json(Submission().chunk));
    EXPECT_EQ(doc.at("draws")[0].at("shader_variant"), Submission().shader_variant);
    EXPECT_EQ(doc.at("draws")[0].at("pass"), 1);
    EXPECT_EQ(doc.at("draws")[0].at("ownership"), 3);
    ASSERT_NE(table.resolve(1), nullptr);
    EXPECT_EQ(*table.resolve(1), first);
    EXPECT_EQ(table.resolve(0), nullptr);
    EXPECT_EQ(table.resolve(4), nullptr);
    EXPECT_TRUE(table.complete_coverage());
    table.clear();
    EXPECT_EQ(table.resolve(1), nullptr);
    EXPECT_TRUE(nlohmann::json::parse(table.json()).at("draws").empty());
    ASSERT_TRUE(table.submit(Submission(), second));
    EXPECT_EQ(second, first);
}

TEST(DiagnosticDrawTable, InvalidMetadataAndHostilePathsRefuseWithoutChangingIdsOrTables) {
    DiagnosticDrawTable table;
    DiagnosticId id{9, 8, 7, 6};
    const auto original = table.json();
    for (const std::string& path : std::vector<std::string>{"",
                                                            "/private/a",
                                                            "C:/a",
                                                            "C:a",
                                                            "a\\b",
                                                            "../a",
                                                            "a/../b",
                                                            "a/./b",
                                                            "a//b",
                                                            "a/",
                                                            "a\nb",
                                                            std::string(1025, 'a'),
                                                            std::string("res/\xff")}) {
        auto submission = Submission();
        submission.mesh_path = path;
        EXPECT_FALSE(table.submit(submission, id));
        EXPECT_EQ(id, (DiagnosticId{9, 8, 7, 6}));
        EXPECT_EQ(table.json(), original);
    }
    for (unsigned int field = 0; field < 7; ++field) {
        auto submission = Submission();
        const std::string oversized(1025, 'a');
        if (field == 0) {
            submission.lod = 256;
        } else if (field == 1) {
            submission.ownership = static_cast<DiagnosticOwnership>(0);
        } else if (field == 2) {
            submission.pass = static_cast<DiagnosticPass>(5);
        } else if (field == 3) {
            submission.provenance = static_cast<DiagnosticProvenance>(255);
        } else if (field == 4) {
            submission.shader_variant = "";
        } else if (field == 5) {
            submission.shader_variant = oversized;
        } else {
            submission.shader_variant = "\xff";
        }
        EXPECT_FALSE(table.submit(submission, id));
        EXPECT_EQ(table.json(), original);
        EXPECT_EQ(id, (DiagnosticId{9, 8, 7, 6}));
    }
    ASSERT_TRUE(table.submit(Submission(), id));
    EXPECT_EQ(id[0], 1U);
    EXPECT_EQ(id[1], 1U);
}

TEST(DiagnosticDrawTable, UnsupportedProducerIsExplicitAndProvenanceSeparatesMeshEntries) {
    DiagnosticDrawTable table;
    auto submission = Submission();
    DiagnosticId id{};
    ASSERT_TRUE(table.submit(submission, id));
    submission.provenance = DiagnosticProvenance::Heightfield;
    ASSERT_TRUE(table.submit(submission, id));
    EXPECT_EQ(id[1], 2U);
    submission.provenance = DiagnosticProvenance::Unsupported;
    submission.mesh_path = "";
    submission.pass = DiagnosticPass::Forward;
    ASSERT_TRUE(table.submit(submission, id));
    EXPECT_EQ(id[0], 3U);
    EXPECT_FALSE(table.complete_coverage());
    const auto doc = nlohmann::json::parse(table.json());
    EXPECT_EQ(doc.at("draws")[2].at("supported"), false);
    EXPECT_EQ(doc.at("meshes")[2].at("provenance"), 4);
    table.clear();
    EXPECT_TRUE(table.complete_coverage());
}

TEST(DiagnosticDrawTable, EntryAndEscapedJsonBudgetsRefuseAtomically) {
    DiagnosticDrawTable table;
    DiagnosticId id{};
    for (std::uint32_t index = 1; index <= 65536; ++index) {
        ASSERT_TRUE(table.submit(Submission(), id));
        ASSERT_EQ(id[0], index);
        ASSERT_EQ(id[1], 1U);
    }
    const auto before = table.json();
    EXPECT_FALSE(table.submit(Submission(), id));
    EXPECT_EQ(table.json(), before);
    EXPECT_EQ(id[0], 65536U);
    table.clear();
    const std::string escaped(1024, '\n');
    auto submission = Submission();
    submission.shader_variant = escaped;
    std::uint32_t accepted = 0;
    // Escaped shader bytes make the JSON cap win before the entry-count cap.
    for (std::uint32_t attempt = 0; attempt < 65536; ++attempt) {
        if (!table.submit(submission, id)) {
            break;
        }
        ++accepted;
    }
    ASSERT_GT(accepted, 0U);
    ASSERT_LT(accepted, 65536U);
    EXPECT_LE(table.json().size(), 16U * 1024U * 1024U);
    const auto capped = table.json();
    const auto previous = id;
    EXPECT_FALSE(table.submit(submission, id));
    EXPECT_EQ(id, previous);
    EXPECT_EQ(table.json(), capped);
}

TEST(DiagnosticDrawTable, UniqueMeshesAndMaximumUtf8PathsRetainFirstSubmissionOrder) {
    DiagnosticDrawTable table;
    DiagnosticId id{};
    auto submission = Submission();
    submission.shader_variant = "v";
    for (std::uint32_t index = 1; index <= 65536; ++index) {
        const auto path = "m/" + std::to_string(index);
        submission.mesh_path = path;
        ASSERT_TRUE(table.submit(submission, id));
        ASSERT_EQ(id[0], index);
        ASSERT_EQ(id[1], index);
    }
    submission.mesh_path = "m/65536";
    EXPECT_FALSE(table.submit(submission, id));
    EXPECT_LE(table.json().size(), 16U * 1024U * 1024U);
    table.clear();
    const std::string path = "res/" + std::string(1018, 'a') + "\xc3\xa9";
    ASSERT_EQ(path.size(), 1024U);
    submission.mesh_path = path;
    ASSERT_TRUE(table.submit(submission, id));
    EXPECT_EQ(nlohmann::json::parse(table.json()).at("meshes")[0].at("path"), path);
}

// The public GL loader entry points let the CPU regression model driver refusal
// and caller state without creating a window or depending on a physical GPU.
class DiagnosticTargetDriver : public testing::Test {
protected:
    struct State {
        GLint texture = 71;
        GLint inactive_texture = 0;
        GLenum active_texture = GL_TEXTURE0;
        GLint units = 2;
        GLint images = 2;
        GLint image_binding = 0;
        GLint draw_fbo = 72;
        GLint read_fbo = 73;
        GLint max_size = 16384;
        GLint max_buffers = 8;
        GLint max_attachments = 8;
        GLint renderable = GL_FULL_SUPPORT;
        GLuint next = 100;
        GLenum status = GL_FRAMEBUFFER_COMPLETE;
        bool zero_texture = false;
        bool zero_fbo = false;
        unsigned int allocations = 0;
        unsigned int deletes = 0;
        unsigned int clears = 0;
        unsigned int queries = 0;
        std::array<GLsizei, 2> size{};
    };
    static State& state() {
        static State driver;
        return driver;
    }
    template<typename Function>
    struct Replace {
        Function& slot;
        Function original;
        Replace(Function& entry, Function replacement)
            : slot(entry)
            , original(entry) {
            slot = replacement;
        }
        ~Replace() {
            slot = original;
        }
    };
    static void APIENTRY Get(GLenum name, GLint* output) {
        auto& driver = state();
        ++driver.queries;
        switch (name) {
            case GL_TEXTURE_BINDING_2D:
                *output =
                    driver.active_texture == GL_TEXTURE0 ? driver.texture : driver.inactive_texture;
                break;
            case GL_ACTIVE_TEXTURE:
                *output = static_cast<GLint>(driver.active_texture);
                break;
            case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
                *output = driver.units;
                break;
            case GL_MAX_IMAGE_UNITS:
                *output = driver.images;
                break;
            case GL_READ_FRAMEBUFFER_BINDING:
                *output = driver.read_fbo;
                break;
            case GL_DRAW_FRAMEBUFFER_BINDING:
                *output = driver.draw_fbo;
                break;
            case GL_MAX_TEXTURE_SIZE:
                *output = driver.max_size;
                break;
            case GL_MAX_DRAW_BUFFERS:
                *output = driver.max_buffers;
                break;
            case GL_MAX_COLOR_ATTACHMENTS:
                *output = driver.max_attachments;
                break;
            default:
                ADD_FAILURE() << name;
                break;
        }
    }
    static void APIENTRY GetIndexed(GLenum name, GLuint index, GLint* output) {
        EXPECT_EQ(name, GL_IMAGE_BINDING_NAME);
        EXPECT_LT(index, static_cast<GLuint>(state().images));
        *output = index == 1 ? state().image_binding : 0;
    }
    static void APIENTRY
    Internal(GLenum target, GLenum format, GLenum query, GLsizei count, GLint* value) {
        EXPECT_EQ(target, GL_TEXTURE_2D);
        EXPECT_EQ(format, GL_RGBA32UI);
        EXPECT_EQ(query, GL_FRAMEBUFFER_RENDERABLE);
        EXPECT_EQ(count, 1);
        *value = state().renderable;
    }
    static void APIENTRY GenTexture(GLsizei count, GLuint* name) {
        EXPECT_EQ(count, 1);
        ++state().allocations;
        *name = state().zero_texture ? 0 : state().next++;
    }
    static void APIENTRY GenFbo(GLsizei count, GLuint* name) {
        EXPECT_EQ(count, 1);
        ++state().allocations;
        *name = state().zero_fbo ? 0 : state().next++;
    }
    static void APIENTRY Delete(GLsizei count, const GLuint* name) {
        EXPECT_EQ(count, 1);
        EXPECT_NE(*name, 0U);
        ++state().deletes;
    }
    static void APIENTRY BindTexture(GLenum target, GLuint name) {
        EXPECT_EQ(target, GL_TEXTURE_2D);
        if (state().active_texture == GL_TEXTURE0) {
            state().texture = static_cast<GLint>(name);
        } else {
            state().inactive_texture = static_cast<GLint>(name);
        }
    }
    static void APIENTRY ActiveTexture(GLenum unit) {
        state().active_texture = unit;
    }
    static void APIENTRY BindFbo(GLenum target, GLuint name) {
        EXPECT_EQ(target, GL_DRAW_FRAMEBUFFER);
        state().draw_fbo = static_cast<GLint>(name);
    }
    static void APIENTRY
    Storage(GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height) {
        EXPECT_EQ(target, GL_TEXTURE_2D);
        EXPECT_EQ(levels, 1);
        EXPECT_EQ(format, GL_RGBA32UI);
        state().size = {width, height};
    }
    static void APIENTRY Parameter(GLenum target, GLenum name, GLint value) {
        EXPECT_EQ(target, GL_TEXTURE_2D);
        EXPECT_EQ(value,
                  name == GL_TEXTURE_MIN_FILTER || name == GL_TEXTURE_MAG_FILTER
                      ? GL_NEAREST
                      : GL_CLAMP_TO_EDGE);
    }
    static void APIENTRY
    Attach(GLenum target, GLenum attachment, GLenum texture_target, GLuint texture, GLint level) {
        EXPECT_EQ(target, GL_DRAW_FRAMEBUFFER);
        EXPECT_EQ(attachment, GL_COLOR_ATTACHMENT0);
        EXPECT_EQ(texture_target, GL_TEXTURE_2D);
        EXPECT_NE(texture, 0U);
        EXPECT_EQ(level, 0);
    }
    static void APIENTRY DrawBuffer(GLenum buffer) {
        EXPECT_EQ(buffer, GL_COLOR_ATTACHMENT0);
    }
    static GLenum APIENTRY Status(GLenum target) {
        EXPECT_EQ(target, GL_DRAW_FRAMEBUFFER);
        return state().status;
    }
    static void APIENTRY
    Clear(GLuint texture, GLint level, GLenum format, GLenum type, const void* value) {
        EXPECT_NE(texture, 0U);
        EXPECT_EQ(level, 0);
        EXPECT_EQ(format, GL_RGBA_INTEGER);
        EXPECT_EQ(type, GL_UNSIGNED_INT);
        EXPECT_EQ(value, nullptr);
        ++state().clears;
    }
    void SetUp() override {
        state() = State{};
    }
    static void ExpectBindings() {
        EXPECT_EQ(state().texture, 71);
        EXPECT_EQ(state().draw_fbo, 72);
        EXPECT_EQ(state().read_fbo, 73);
        EXPECT_EQ(state().active_texture, GL_TEXTURE0);
        EXPECT_EQ(state().inactive_texture, 0);
    }
    Replace<PFNGLGETINTEGERVPROC> get_{glad_glGetIntegerv, Get};
    Replace<PFNGLGETINTEGERI_VPROC> get_indexed_{glad_glGetIntegeri_v, GetIndexed};
    Replace<PFNGLGETINTERNALFORMATIVPROC> internal_{glad_glGetInternalformativ, Internal};
    Replace<PFNGLGENTEXTURESPROC> gen_texture_{glad_glGenTextures, GenTexture};
    Replace<PFNGLGENFRAMEBUFFERSPROC> gen_fbo_{glad_glGenFramebuffers, GenFbo};
    Replace<PFNGLDELETETEXTURESPROC> delete_texture_{glad_glDeleteTextures, Delete};
    Replace<PFNGLDELETEFRAMEBUFFERSPROC> delete_fbo_{glad_glDeleteFramebuffers, Delete};
    Replace<PFNGLACTIVETEXTUREPROC> active_texture_{glad_glActiveTexture, ActiveTexture};
    Replace<PFNGLBINDTEXTUREPROC> bind_texture_{glad_glBindTexture, BindTexture};
    Replace<PFNGLBINDFRAMEBUFFERPROC> bind_fbo_{glad_glBindFramebuffer, BindFbo};
    Replace<PFNGLTEXSTORAGE2DPROC> storage_{glad_glTexStorage2D, Storage};
    Replace<PFNGLTEXPARAMETERIPROC> parameter_{glad_glTexParameteri, Parameter};
    Replace<PFNGLFRAMEBUFFERTEXTURE2DPROC> attach_{glad_glFramebufferTexture2D, Attach};
    Replace<PFNGLDRAWBUFFERPROC> draw_{glad_glDrawBuffer, DrawBuffer};
    Replace<PFNGLCHECKFRAMEBUFFERSTATUSPROC> status_{glad_glCheckFramebufferStatus, Status};
    Replace<PFNGLCLEARTEXIMAGEPROC> clear_{glad_glClearTexImage, Clear};
};

TEST_F(DiagnosticTargetDriver, OffIsLazyAndResizeReusesOrReplacesWithBindingsRestored) {
    DiagnosticTargets targets;
    EXPECT_TRUE(targets.ensure(false, {}));
    EXPECT_EQ(state().queries, 0U);
    EXPECT_EQ(state().allocations, 0U);
    EXPECT_EQ(targets.texture(), 0U);
    ASSERT_TRUE(targets.ensure(true, {4, 3}, 6));
    EXPECT_EQ(state().size, (std::array<GLsizei, 2>{4, 3}));
    EXPECT_EQ(state().clears, 1U);
    ExpectBindings();
    const auto old_texture = targets.texture();
    const auto old_fbo = targets.framebuffer();
    ASSERT_TRUE(targets.ensure(true, {4, 3}, 6));
    EXPECT_EQ(state().allocations, 2U);
    EXPECT_EQ(targets.texture(), old_texture);
    ASSERT_TRUE(targets.ensure(true, {8, 2}, 6));
    EXPECT_NE(targets.texture(), old_texture);
    EXPECT_NE(targets.framebuffer(), old_fbo);
    EXPECT_EQ(state().deletes, 2U);
    EXPECT_EQ(state().clears, 2U);
    ExpectBindings();
    EXPECT_TRUE(targets.ensure(false, {}));
    EXPECT_EQ(state().deletes, 4U);
    EXPECT_EQ(targets.texture(), 0U);
    EXPECT_EQ(targets.framebuffer(), 0U);
    EXPECT_EQ(targets.extent().width, 0U);
    targets.reset();
    EXPECT_EQ(state().deletes, 4U);
}

TEST_F(DiagnosticTargetDriver, InvalidExtentsAndCapabilitiesRefuseBeforeAllocation) {
    DiagnosticTargets targets;
    for (const auto& extent : std::array<DiagnosticExtent, 7>{{{0, 1},
                                                               {1, 0},
                                                               {16385, 1},
                                                               {1, 16385},
                                                               {4096, 4097},
                                                               {16384, 16384},
                                                               {0xffffffffU, 0xffffffffU}}}) {
        EXPECT_FALSE(targets.ensure(true, extent));
    }
    EXPECT_EQ(state().queries, 0U);
    EXPECT_FALSE(targets.ensure(true, {1, 1}, 0));
    for (unsigned int failure = 0; failure < 4; ++failure) {
        state() = State{};
        if (failure == 0) {
            state().max_size = 1;
        }
        if (failure == 1) {
            state().max_buffers = 5;
        }
        if (failure == 2) {
            state().max_attachments = 5;
        }
        if (failure == 3) {
            state().renderable = GL_NONE;
        }
        EXPECT_FALSE(targets.ensure(true, {2, 2}, 6));
        EXPECT_EQ(state().allocations, 0U);
        ExpectBindings();
    }
    state() = State{};
    EXPECT_TRUE(targets.ensure(true, {16384, 1024}, 8));
    ExpectBindings();
}

TEST_F(DiagnosticTargetDriver, MissingIntegerStorageOrClearEntryPointsRefuseBeforeQueries) {
    DiagnosticTargets targets;
    {
        Replace<PFNGLGETINTERNALFORMATIVPROC> missing(glad_glGetInternalformativ, nullptr);
        EXPECT_FALSE(targets.ensure(true, {2, 2}));
    }
    {
        Replace<PFNGLTEXSTORAGE2DPROC> missing(glad_glTexStorage2D, nullptr);
        EXPECT_FALSE(targets.ensure(true, {2, 2}));
    }
    {
        Replace<PFNGLCLEARTEXIMAGEPROC> missing(glad_glClearTexImage, nullptr);
        EXPECT_FALSE(targets.ensure(true, {2, 2}));
    }
    EXPECT_EQ(state().queries, 0U);
    EXPECT_EQ(state().allocations, 0U);
    ExpectBindings();
}

TEST_F(DiagnosticTargetDriver, NondefaultActiveTextureAndCaveatSupportPreserveCallerState) {
    state().active_texture = GL_TEXTURE1;
    state().inactive_texture = 83;
    state().renderable = GL_CAVEAT_SUPPORT;
    DiagnosticTargets targets;
    ASSERT_TRUE(targets.ensure(true, {2, 2}));
    ASSERT_TRUE(targets.ensure(true, {3, 3}));
    EXPECT_EQ(state().active_texture, GL_TEXTURE1);
    EXPECT_EQ(state().texture, 71);
    EXPECT_EQ(state().inactive_texture, 83);
    EXPECT_EQ(state().draw_fbo, 72);
    EXPECT_EQ(state().read_fbo, 73);
}

TEST_F(DiagnosticTargetDriver, BoundOwnedStorageRefusesResizeWithoutRetiringOrChangingBindings) {
    for (unsigned int binding = 0; binding < 8; ++binding) {
        state() = State{};
        DiagnosticTargets targets;
        ASSERT_TRUE(targets.ensure(true, {2, 2}));
        const auto texture = targets.texture();
        const auto framebuffer = targets.framebuffer();
        if (binding == 0) {
            state().texture = static_cast<GLint>(texture);
        } else if (binding == 1) {
            state().draw_fbo = static_cast<GLint>(framebuffer);
        } else if (binding == 2) {
            state().read_fbo = static_cast<GLint>(framebuffer);
        } else if (binding == 3) {
            state().inactive_texture = static_cast<GLint>(texture);
        } else if (binding == 6) {
            state().image_binding = static_cast<GLint>(texture);
        } else if (binding == 7) {
            state().images = 257;
        } else {
            state().units = binding == 4 ? 0 : 257;
        }
        const auto previous_texture = state().texture;
        const auto previous_draw = state().draw_fbo;
        const auto previous_read = state().read_fbo;
        EXPECT_FALSE(targets.ensure(true, {3, 3}));
        EXPECT_EQ(state().texture, previous_texture);
        EXPECT_EQ(state().draw_fbo, previous_draw);
        EXPECT_EQ(state().read_fbo, previous_read);
        EXPECT_EQ(state().active_texture, GL_TEXTURE0);
        if (binding == 3) {
            EXPECT_EQ(state().inactive_texture, static_cast<GLint>(texture));
        }
        EXPECT_EQ(targets.texture(), texture);
        EXPECT_EQ(targets.framebuffer(), framebuffer);
        EXPECT_EQ(state().deletes, 0U);
        EXPECT_EQ(state().allocations, 2U);
        state().texture = 71;
        state().draw_fbo = 72;
        state().read_fbo = 73;
    }
}

TEST_F(DiagnosticTargetDriver, PartialAllocationAndIncompleteResizeKeepPreviousTargets) {
    for (unsigned int failure = 0; failure < 3; ++failure) {
        state() = State{};
        DiagnosticTargets targets;
        ASSERT_TRUE(targets.ensure(true, {2, 2}));
        const auto texture = targets.texture();
        const auto framebuffer = targets.framebuffer();
        state().zero_texture = failure == 0;
        state().zero_fbo = failure == 1;
        state().status =
            failure == 2 ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT : GL_FRAMEBUFFER_COMPLETE;
        EXPECT_FALSE(targets.ensure(true, {3, 3}));
        EXPECT_EQ(targets.texture(), texture);
        EXPECT_EQ(targets.framebuffer(), framebuffer);
        EXPECT_EQ(targets.extent().width, 2U);
        EXPECT_EQ(state().deletes, failure == 2 ? 2U : 1U);
        EXPECT_EQ(state().clears, 1U);
        ExpectBindings();
    }
}

} // namespace
