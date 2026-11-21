#include "luminumbra_client/rendering/IndexedUniformNames.h"
#include "luminumbra_client/rendering/ShaderSource.h"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string>

namespace {
using Luminumbra::Rendering::InjectShaderDefines;
const std::array<std::string, 1> kDefines{"LUMINUMBRA_FAR_WORLD_LADDER 1"};

struct HeaderCase {
    const char* source;
    const char* expected;
};

TEST(ShaderSource, DefinesFollowRealVersionAndRestoreOriginalLines) {
    const std::array cases{
        HeaderCase{" \t\n\n#version 450 core\nvoid main() {}\n",
                   " \t\n\n#version 450 core\n#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 4\nvoid "
                   "main() {}\n"},
        HeaderCase{"// #version 110 is a comment\n#version 450 core\nvoid main() {}\n",
                   "// #version 110 is a comment\n#version 450 core\n#define "
                   "LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 3\nvoid main() {}\n"},
        HeaderCase{"/* leading\n#version 110\ncomment */\n#version 450 core\nvoid main() {}\n",
                   "/* leading\n#version 110\ncomment */\n#version 450 core\n#define "
                   "LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 5\nvoid main() {}\n"},
        HeaderCase{"// header\r\n#version 450 core\r\nvoid main() {}\r\n",
                   "// header\r\n#version 450 core\r\n#define LUMINUMBRA_FAR_WORLD_LADDER "
                   "1\r\n#line 3\r\nvoid main() {}\r\n"},
        HeaderCase{"#version 450 core // trailing #version 110\nvoid main() {}\n",
                   "#version 450 core // trailing #version 110\n#define "
                   "LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 2\nvoid main() {}\n"},
        HeaderCase{
            "/* no version */\nvoid main() {}\n",
            "#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 1\n/* no version */\nvoid main() {}\n"},
        HeaderCase{"#version 450 core /* trailing\n#version 110\ncomment */\nvoid main() {}\n",
                   "#version 450 core /* trailing\n#version 110\ncomment */\n#define "
                   "LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 4\nvoid main() {}\n"},
        HeaderCase{"# /* trivia */ version 450 core\nvoid main() {}\n",
                   "# /* trivia */ version 450 core\n#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line "
                   "2\nvoid main() {}\n"},
        HeaderCase{"#version 450 core",
                   "#version 450 core\n#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 2\n"},
        HeaderCase{
            "void main() {}\n#version 450 core\n",
            "#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 1\nvoid main() {}\n#version 450 core\n"},
        HeaderCase{"#versioned 450\n",
                   "#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 1\n#versioned 450\n"},
        HeaderCase{"/* comment */\r\nvoid main() {}\r\n",
                   "#define LUMINUMBRA_FAR_WORLD_LADDER 1\r\n#line 1\r\n/* comment */\r\nvoid "
                   "main() {}\r\n"},
        HeaderCase{"", "#define LUMINUMBRA_FAR_WORLD_LADDER 1\n#line 1\n"}};
    for (const auto& item : cases) {
        SCOPED_TRACE(item.source);
        std::string source = item.source, diagnostic;
        ASSERT_TRUE(InjectShaderDefines(source, kDefines, diagnostic)) << diagnostic;
        EXPECT_EQ(source, item.expected);
        // The no-define production path performs no lexer edits, even on CRLF,
        // a missing final newline, or a versionless source.
        source = item.source;
        ASSERT_TRUE(InjectShaderDefines(source, {}, diagnostic));
        EXPECT_EQ(source, item.source);
    }
}

TEST(ShaderSource, UnterminatedBlockCommentRefusesWithoutMutatingSource) {
    for (const char* text : {"/* leading\n#version 450 core\n",
                             "#version 450 core /* trailing\n",
                             "#version 450 core\nvoid main() {}\n/* unterminated",
                             "void main() {}\n/* unterminated"}) {
        SCOPED_TRACE(text);
        std::string source = text, diagnostic;
        EXPECT_FALSE(InjectShaderDefines(source, kDefines, diagnostic));
        EXPECT_EQ(source, text);
        EXPECT_NE(diagnostic.find("unterminated GLSL block comment at line "), std::string::npos);
    }
}

TEST(ShaderSource, CommentMarkersInLineCommentsAndQuotedOperandsAreInert) {
    std::string source = "// /* not a block comment\n#version 450 core\n"
                         "#define TEXT \"/* not a block comment\"\nvoid main() {}\n";
    std::string diagnostic;
    ASSERT_TRUE(InjectShaderDefines(source, kDefines, diagnostic)) << diagnostic;
    EXPECT_NE(source.find("#line 3\n#define TEXT"), std::string::npos);
}

TEST(ShaderSource, MultipleDefinesUseVersionLineEndingAndPreserveBodyLine) {
    std::string source = "// LF header\n#version 450 core\r\nvoid main() {}";
    const std::array<std::string, 2> defines{"FIRST 1", "SECOND 2"};
    std::string diagnostic;
    ASSERT_TRUE(InjectShaderDefines(source, defines, diagnostic)) << diagnostic;
    EXPECT_EQ(source,
              "// LF header\n#version 450 core\r\n"
              "#define FIRST 1\r\n#define SECOND 2\r\n#line 3\r\nvoid main() {}");
}
} // namespace

namespace {
using Luminumbra::Rendering::IndexedUniformNames;

template<std::size_t Count, std::size_t Fields>
void CheckIndexedNames(const std::array<std::string, Fields>& prefixes,
                       const std::array<std::string, Fields>& suffixes) {
    IndexedUniformNames<Count, Fields> cache;
    std::array<std::size_t, Count> builds{};
    const auto build = [&](std::size_t index) {
        ++builds[index];
        std::array<std::string, Fields> names{};
        for (std::size_t field = 0; field < Fields; ++field)
            names[field] = prefixes[field] + std::to_string(index) + suffixes[field];
        return names;
    };
    // Empty frames build nothing; shrinking/growing and reverse upload order
    // still build each accessed name only once in the current generation.
    for (std::uint64_t generation : {0U, 1U, 2U}) {
        for (std::size_t frame = 0; frame < 4; ++frame) {
            const auto count = frame == 0 ? 0 : (frame == 1 ? Count / 2 : Count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto index = count - offset - 1;
                const auto& names = cache.Get(generation, index, build);
                for (std::size_t field = 0; field < Fields; ++field)
                    EXPECT_EQ(names[field],
                              prefixes[field] + std::to_string(index) + suffixes[field]);
            }
        }
        for (const auto count : builds)
            EXPECT_EQ(count, generation + 1);
        // Failed reload keeps the generation, so even another full frame is free.
        for (std::size_t index = 0; index < Count; ++index) {
            cache.Get(generation, index, build);
            EXPECT_EQ(builds[index], generation + 1);
        }
    }
    cache.Reset(); // replacement shader can restart at the same generation
    for (std::size_t index = 0; index < Count; ++index) {
        cache.Get(2, index, build);
        cache.Get(2, index, build);
        EXPECT_EQ(builds[index], 4U);
    }
}

TEST(IndexedUniformNames, EveryShippingFamilyBuildsOncePerGeneration) {
    CheckIndexedNames<64, 1>({"u_samples["}, {"]"});
    CheckIndexedNames<32, 4>(
        {"u_pointLights[", "u_pointLights[", "u_pointLights[", "u_pointLights["},
        {"].position", "].color", "].radius", "].intensity"});
    CheckIndexedNames<4, 1>({"u_lightSpaceMatrices["}, {"]"});
    CheckIndexedNames<8, 3>({"u_flashNdc[", "u_flashPulse[", "u_flashOnscreen["}, {"]", "]", "]"});
    CheckIndexedNames<96, 2>({"u_bolt[", "u_boltInverseDepth["}, {"]", "]"});
}

TEST(IndexedUniformNames, ShaderInstancesRetainIndependentGenerations) {
    IndexedUniformNames<1> normal, contribution;
    std::size_t builds = 0;
    const auto build = [&](std::size_t) {
        ++builds;
        return IndexedUniformNames<1>::Names{"u_samples[0]"};
    };
    for (int frame = 0; frame < 4; ++frame) {
        normal.Get(1, 0, build);
        contribution.Get(1, 0, build);
    }
    EXPECT_EQ(builds, 2U);
    normal.Get(2, 0, build);
    contribution.Get(1, 0, build);
    EXPECT_EQ(builds, 3U);
}

TEST(IndexedUniformNames, RefusedIndexAndFailedBuildDoNotPublishEntries) {
    IndexedUniformNames<1> cache;
    std::size_t builds = 0;
    bool refuse = true;
    const auto build = [&](std::size_t) -> IndexedUniformNames<1>::Names {
        ++builds;
        if (refuse)
            throw std::runtime_error("name build refused");
        return {"u_samples[0]"};
    };
    EXPECT_THROW(cache.Get(1, 1, build), std::out_of_range);
    EXPECT_THROW(cache.Get(1, static_cast<std::size_t>(-1), build), std::out_of_range);
    EXPECT_EQ(builds, 0U);
    EXPECT_THROW(cache.Get(1, 0, build), std::runtime_error);
    refuse = false;
    EXPECT_EQ(cache.Get(1, 0, build)[0], "u_samples[0]");
    EXPECT_EQ(builds, 2U);
    refuse = true;
    EXPECT_EQ(cache.Get(1, 0, build)[0], "u_samples[0]");
    EXPECT_THROW(cache.Get(2, 0, build), std::runtime_error);
    // An unsuccessful generation change leaves the previous entry usable.
    EXPECT_EQ(cache.Get(1, 0, build)[0], "u_samples[0]");
    refuse = false;
    EXPECT_EQ(cache.Get(2, 0, build)[0], "u_samples[0]");
    EXPECT_EQ(builds, 4U);
    IndexedUniformNames<0> empty;
    EXPECT_THROW(empty.Get(0, 0, build), std::out_of_range);
    EXPECT_EQ(builds, 4U);
}
} // namespace
