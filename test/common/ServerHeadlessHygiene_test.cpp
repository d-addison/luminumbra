// headless server hygiene gate. The luminumbra_server_app target
// links luminumbra_common ONLY; nothing under src/luminumbra_server/ may
// include any client-side library (OpenGL/glad, GLFW, miniaudio, imgui,
// RmlUi, SOIL2) or reach into luminumbra_client. This test scans every
// source/header under src/luminumbra_server for forbidden #include targets
// so a violation fails ctest before it ever reaches a link line.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsScannedSourceFile(const fs::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".h" || ext == ".hpp" || ext == ".cpp" || ext == ".inl" || ext == ".c";
}

// Lowercased substrings that no #include target under src/luminumbra_server
// may contain. "glm" stays allowed (math library used by luminumbra_common),
// hence the GL patterns match "gl/gl..." and "opengl" rather than bare "gl".
const std::vector<std::string>& ForbiddenIncludeTokens() {
    static const std::vector<std::string> tokens = {
        "glfw",
        "glad",
        "imgui",
        "miniaudio",
        "rmlui",
        "rml/",
        "soil2",
        "opengl",
        "gl/gl",
        "gles",
        "luminumbra_client",
    };
    return tokens;
}

TEST(ServerHeadlessHygieneTest, ServerSourcesContainNoClientLibraryIncludes) {
    const fs::path server_root = fs::path(LUMINUMBRA_SOURCE_ROOT) / "src" / "luminumbra_server";
    ASSERT_TRUE(fs::exists(server_root)) << "missing " << server_root.string();

    const std::regex include_pattern(R"(^\s*#\s*include\s*[<"]([^">]+)[">])");

    std::size_t files_scanned = 0;
    std::vector<std::string> violations;

    for (const auto& entry : fs::recursive_directory_iterator(server_root)) {
        if (!entry.is_regular_file() || !IsScannedSourceFile(entry.path())) {
            continue;
        }
        ++files_scanned;

        std::ifstream file(entry.path());
        ASSERT_TRUE(file.is_open()) << "failed to open " << entry.path().string();

        std::string line;
        std::size_t line_number = 0;
        while (std::getline(file, line)) {
            ++line_number;
            std::smatch match;
            if (!std::regex_search(line, match, include_pattern)) {
                continue;
            }
            const std::string include_target = ToLower(match[1].str());
            for (const std::string& token : ForbiddenIncludeTokens()) {
                if (include_target.find(token) != std::string::npos) {
                    violations.push_back(entry.path().string() + ":" + std::to_string(line_number) +
                                         " includes forbidden client dependency '" +
                                         match[1].str() + "'");
                }
            }
        }
    }

    // The gate must actually scan the server sources: a path regression that
    // finds zero files would otherwise pass vacuously.
    EXPECT_GE(files_scanned, 1u) << "no server sources found under " << server_root.string();

    std::string report;
    for (const std::string& violation : violations) {
        report += violation + "\n";
    }
    EXPECT_TRUE(violations.empty()) << report;
}

} // namespace
