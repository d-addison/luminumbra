#include "luminumbra_common/core/CrashReport.h"
#include "luminumbra_server/ServerCliOptions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
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

#if !defined(_WIN32)
std::string ReadAll(const fs::path& file) {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

} // namespace

TEST(CrashReportTest, ExplicitCrashDirectoryOverridesEnvironment) {
    EXPECT_EQ(ResolveServerCrashDirectory("explicit/crash", "environment/crash"),
              fs::path("explicit/crash"));
}

TEST(CrashReportTest, EnvironmentCrashDirectoryIsUsedForCapture) {
    ScopedDirectory scratch(UniqueScratchDir("environment"));
    const fs::path dir = scratch.path / "environment" / "crash";
    const std::string environment_directory = dir.string();
    const fs::path resolved = ResolveServerCrashDirectory("", environment_directory.c_str());
    EXPECT_EQ(resolved, dir);
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(resolved, "crash_report_test"));
    EXPECT_EQ(luminumbra::crash::CrashReportPath().parent_path(), dir);
}

TEST(CrashReportTest, NullOrEmptyEnvironmentUsesDefaultCrashDirectory) {
    EXPECT_EQ(ResolveServerCrashDirectory("", nullptr), fs::path("logs") / "crash");
    EXPECT_EQ(ResolveServerCrashDirectory("", ""), fs::path("logs") / "crash");
}

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

TEST(CrashReportTest, ReinstallIntoSameDirectoryReplacesReport) {
    ScopedDirectory scratch(UniqueScratchDir("reinstall"));
    const fs::path dir = scratch.path / "crash";

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    const fs::path first = luminumbra::crash::CrashReportPath();

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    const fs::path second = luminumbra::crash::CrashReportPath();

    EXPECT_EQ(first, second);
    EXPECT_TRUE(fs::is_regular_file(second));
    EXPECT_EQ(FirstLine(second), kReportHeader);
}

#if !defined(_WIN32)

constexpr const char* kChildDirEnv = "LUMINUMBRA_CRASH_CHILD";
constexpr const char* kChildModeEnv = "LUMINUMBRA_CRASH_CHILD_MODE";
constexpr const char* kChildSegv = "segv";
constexpr const char* kChildAbort = "abort";

// Runs only as a child process spawned by the parent tests below. Without the
// environment variables it passes immediately, so it is harmless in a normal run.
TEST(CrashReportChild, CrashesWhenRequested) {
    const char* crash_dir = std::getenv(kChildDirEnv);
    const char* mode = std::getenv(kChildModeEnv);
    if (crash_dir == nullptr || mode == nullptr) {
        return;
    }
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(crash_dir, "crash_report_child"));
    if (std::string(mode) == kChildAbort) {
        std::raise(SIGABRT);
        return;
    }
    // A volatile store cannot be removed or reordered away by the optimizer.
    volatile int* volatile null_pointer = nullptr;
    *null_pointer = 1;
}

// Waits for the child with a deadline. On expiry the child is killed and false is returned.
bool WaitForChild(pid_t pid, int& status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return true;
        }
        if (waited < 0) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ExpectChildCrashReport(const char* mode, int expected_signal, const char* signal_name) {
    std::string self(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", self.data(), self.size());
    ASSERT_GT(length, 0);
    self.resize(static_cast<std::size_t>(length));

    ScopedDirectory scratch(UniqueScratchDir("child"));
    fs::create_directories(scratch.path);
    std::string child_dir_environment = std::string(kChildDirEnv) + "=" + scratch.path.string();
    std::string child_mode_environment = std::string(kChildModeEnv) + "=" + mode;
    std::vector<char*> child_environment;
    for (char** entry = ::environ; *entry != nullptr; ++entry) {
        const std::string_view value(*entry);
        const std::string_view name = value.substr(0, value.find('='));
        if (name != kChildDirEnv && name != kChildModeEnv) {
            child_environment.push_back(*entry);
        }
    }
    child_environment.push_back(child_dir_environment.data());
    child_environment.push_back(child_mode_environment.data());
    child_environment.push_back(nullptr);
    const std::string filter_arg = "--gtest_filter=CrashReportChild.*";

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::execle(self.c_str(),
                 self.c_str(),
                 filter_arg.c_str(),
                 static_cast<char*>(nullptr),
                 child_environment.data());
        ::_exit(127);
    }
    int status = 0;
    const bool exited = WaitForChild(pid, status);
    ASSERT_TRUE(exited) << "child did not exit within 30 seconds; it was killed";

    ASSERT_TRUE(WIFSIGNALED(status)) << "child exited normally with status " << WEXITSTATUS(status);
    EXPECT_EQ(WTERMSIG(status), expected_signal);

    // Only the report named for the child's own pid counts.
    const fs::path report = scratch.path / ("crash-" + std::to_string(pid) + ".txt");
    ASSERT_TRUE(fs::is_regular_file(report)) << "no report for child pid " << pid;
    EXPECT_EQ(FirstLine(report), kReportHeader);

    const std::string contents = ReadAll(report);
    const std::string signal_line =
        "\nsignal " + std::to_string(expected_signal) + " " + signal_name + "\n";
    EXPECT_NE(contents.find(signal_line), std::string::npos) << contents;
    EXPECT_NE(contents.find("\nfault_address 0x"), std::string::npos) << contents;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    EXPECT_NE(contents.find("\npc 0x"), std::string::npos) << contents;
    EXPECT_NE(contents.find("\nsp 0x"), std::string::npos) << contents;
#endif
}

TEST(CrashReportTest, ChildSegfaultWritesSignalRecordAndDiesBySigsegv) {
    ExpectChildCrashReport(kChildSegv, SIGSEGV, "SIGSEGV");
}

TEST(CrashReportTest, ChildRaisedSigabrtWritesSignalRecordAndDiesBySigabrt) {
    ExpectChildCrashReport(kChildAbort, SIGABRT, "SIGABRT");
}

#endif
