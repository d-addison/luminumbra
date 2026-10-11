#include "luminumbra_common/core/CrashReport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kReportHeader = "luminumbra-crash-report v1";

fs::path UniqueScratchDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           (std::string("luminumbra-crash-") + tag + "-" + std::to_string(stamp));
}

struct ScopedDirectory {
    explicit ScopedDirectory(fs::path p)
        : path(std::move(p)) {}
    ~ScopedDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

std::string FirstLine(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

} // namespace

TEST(CrashReportTest, InstallCreatesDirectoryAndOpenableReportFile) {
    ScopedDirectory scratch(UniqueScratchDir("install"));
    const fs::path dir = scratch.path / "nested" / "crash";

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    EXPECT_TRUE(fs::is_directory(dir));

    const fs::path report = luminumbra::crash::CrashReportPath();
    EXPECT_EQ(report.parent_path(), dir);
    EXPECT_TRUE(fs::is_regular_file(report));
    EXPECT_EQ(FirstLine(report), kReportHeader);
}

TEST(CrashReportTest, InstallReturnsFalseWhenDirectoryCannotBeCreated) {
    ScopedDirectory scratch(UniqueScratchDir("badpath"));
    fs::create_directories(scratch.path);
    const fs::path blocker = scratch.path / "blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory";
    }

    EXPECT_FALSE(luminumbra::crash::InstallCrashCapture(blocker / "crash", "crash_report_test"));
}

#ifndef _WIN32

constexpr const char* kChildEnv = "LUMINUMBRA_CRASH_CHILD";

// Runs only as a child process spawned by CrashReportTest.ChildProcessDiesBySignalAndWritesReport.
// Without the environment variable it passes immediately, so it is harmless in a normal run.
TEST(CrashReportChild, CrashesWhenRequested) {
    const char* crash_dir = std::getenv(kChildEnv);
    if (crash_dir == nullptr) {
        return;
    }
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(crash_dir, "crash_report_child"));
    int* volatile null_pointer = nullptr;
    *null_pointer = 1;
}

TEST(CrashReportTest, ChildProcessDiesBySignalAndWritesReport) {
    std::string self(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", self.data(), self.size());
    ASSERT_GT(length, 0);
    self.resize(static_cast<std::size_t>(length));

    ScopedDirectory scratch(UniqueScratchDir("child"));
    fs::create_directories(scratch.path);
    const std::string child_dir = scratch.path.string();
    const std::string filter_arg = "--gtest_filter=CrashReportChild.*";

    ASSERT_EQ(::setenv(kChildEnv, child_dir.c_str(), 1), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::execl(self.c_str(), self.c_str(), filter_arg.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, 0);
    ::unsetenv(kChildEnv);
    ASSERT_EQ(waited, pid);

    ASSERT_TRUE(WIFSIGNALED(status)) << "child exited normally with status " << WEXITSTATUS(status);

    fs::path report;
    for (const auto& entry : fs::directory_iterator(scratch.path)) {
        if (entry.path().filename().string().rfind("crash-", 0) == 0) {
            report = entry.path();
        }
    }
    ASSERT_FALSE(report.empty());
    EXPECT_EQ(FirstLine(report), kReportHeader);

    std::ifstream in(report);
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("signal"), std::string::npos);
}

#endif
