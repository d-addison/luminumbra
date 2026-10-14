#include "luminumbra_client/rendering/ShaderSource.h"

#include <gtest/gtest.h>

#include <array>
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
