#include "luminumbra_common/core/CrashReport.h"
#include "luminumbra_server/ServerCliOptions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#endif
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

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Each installer test owns a fresh process: fixed target slots must not accumulate
// across independent fixtures or gtest repetitions. Return from the helper before
// exiting so its scratch directories are cleaned, even after a fatal assertion.
void RunCaptureChecks(void (*checks)()) {
    checks();
    std::_Exit(::testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
}

std::optional<std::size_t> OpenDescriptorCount() {
#if defined(__linux__)
    std::error_code ec;
    std::size_t count = 0;
    for (fs::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) {
        ++count;
    }
    if (!ec) {
        return count;
    }
#endif
    return std::nullopt;
}

} // namespace

TEST(CrashReportTest, ExplicitCrashDirectoryOverridesEnvironment) {
    EXPECT_EQ(ResolveServerCrashDirectory("explicit/crash", "environment/crash"),
              fs::path("explicit/crash"));
}

void CheckEnvironmentCrashDirectoryIsUsedForCapture() {
    ScopedDirectory scratch(UniqueScratchDir("environment"));
    const fs::path dir = scratch.path / "environment" / "crash";
    const std::string environment_directory = dir.string();
    const fs::path resolved = ResolveServerCrashDirectory("", environment_directory.c_str());
    EXPECT_EQ(resolved, dir);
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(resolved, "crash_report_test"));
    EXPECT_EQ(luminumbra::crash::CrashReportPath().parent_path(), dir);
}

TEST(CrashReportTest, EnvironmentCrashDirectoryIsUsedForCapture) {
    ASSERT_EXIT(RunCaptureChecks(&CheckEnvironmentCrashDirectoryIsUsedForCapture),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

TEST(CrashReportTest, NullOrEmptyEnvironmentUsesDefaultCrashDirectory) {
    EXPECT_EQ(ResolveServerCrashDirectory("", nullptr), fs::path("logs") / "crash");
    EXPECT_EQ(ResolveServerCrashDirectory("", ""), fs::path("logs") / "crash");
}

void CheckInstallCreatesDirectoryAndOpenableReportFile() {
    ScopedDirectory scratch(UniqueScratchDir("install"));
    const fs::path dir = scratch.path / "nested" / "crash";

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    EXPECT_TRUE(fs::is_directory(dir));

    const fs::path report = luminumbra::crash::CrashReportPath();
    EXPECT_EQ(report.parent_path(), dir);
    EXPECT_TRUE(fs::is_regular_file(report));
    EXPECT_EQ(FirstLine(report), kReportHeader);
}

TEST(CrashReportTest, InstallCreatesDirectoryAndOpenableReportFile) {
    ASSERT_EXIT(RunCaptureChecks(&CheckInstallCreatesDirectoryAndOpenableReportFile),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

void CheckInstallReturnsFalseWhenDirectoryCannotBeCreated() {
    ScopedDirectory scratch(UniqueScratchDir("badpath"));
    fs::create_directories(scratch.path);
    const fs::path blocker = scratch.path / "blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory";
    }

    EXPECT_FALSE(luminumbra::crash::InstallCrashCapture(blocker / "crash", "crash_report_test"));
}

TEST(CrashReportTest, InstallReturnsFalseWhenDirectoryCannotBeCreated) {
    ASSERT_EXIT(RunCaptureChecks(&CheckInstallReturnsFalseWhenDirectoryCannotBeCreated),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

void CheckReinstallIntoSameDirectoryPreservesReport() {
    ScopedDirectory scratch(UniqueScratchDir("reinstall"));
    const fs::path dir = scratch.path / "crash";

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    const fs::path first = luminumbra::crash::CrashReportPath();
    {
        std::ofstream out(first, std::ios::app);
        out << "signal 11 SIGSEGV\nfault_address 0x1234\n";
        ASSERT_TRUE(out.good());
    }
    const std::string before = ReadAll(first);

    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "crash_report_test"));
    EXPECT_EQ(ReadAll(first), before);
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(dir, "a_longer_reinstalled_program_name"));
    const fs::path second = luminumbra::crash::CrashReportPath();

    EXPECT_EQ(first, second);
    EXPECT_TRUE(fs::is_regular_file(second));
    EXPECT_EQ(FirstLine(second), kReportHeader);
    EXPECT_EQ(ReadAll(second), before);
}

TEST(CrashReportTest, ReinstallIntoSameDirectoryPreservesReport) {
    ASSERT_EXIT(RunCaptureChecks(&CheckReinstallIntoSameDirectoryPreservesReport),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

void CheckRepeatedSameThreadReinstallDoesNotGrowDescriptorCount() {
    ScopedDirectory scratch(UniqueScratchDir("descriptor-count"));
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "original"));
    const fs::path report = luminumbra::crash::CrashReportPath();
    const std::string before = ReadAll(report);
    const auto initial_count = OpenDescriptorCount();
    for (int iteration = 0; iteration < 64; ++iteration) {
        ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "reinstalled"));
        EXPECT_EQ(luminumbra::crash::CrashReportPath(), report);
        if (initial_count) {
            EXPECT_EQ(OpenDescriptorCount(), initial_count);
        }
    }
    EXPECT_EQ(ReadAll(report), before);
}

TEST(CrashReportTest, RepeatedSameThreadReinstallDoesNotGrowDescriptorCount) {
    ASSERT_EXIT(RunCaptureChecks(&CheckRepeatedSameThreadReinstallDoesNotGrowDescriptorCount),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

void CheckRecreatedReportUsesFreshFileAndPreservesOldTarget() {
    ScopedDirectory scratch(UniqueScratchDir("recreated"));
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "original"));
    const fs::path report = luminumbra::crash::CrashReportPath();
    const fs::path archived = scratch.path / "archived.txt";
    const std::string before = ReadAll(report);
    fs::rename(report, archived);
    // A pre-existing replacement (including a hard link) must never be truncated.
    {
        std::ofstream out(report);
        out << "existing crash evidence\n";
        ASSERT_TRUE(out.good());
    }
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "replacement"));
    EXPECT_NE(luminumbra::crash::CrashReportPath(), report);
    EXPECT_EQ(ReadAll(report), "existing crash evidence\n");
    EXPECT_EQ(ReadAll(archived), before);
    EXPECT_EQ(FirstLine(luminumbra::crash::CrashReportPath()), kReportHeader);
    EXPECT_FALSE(fs::equivalent(archived, luminumbra::crash::CrashReportPath()));
}

TEST(CrashReportTest, RecreatedReportUsesFreshFileAndPreservesOldTarget) {
    ASSERT_EXIT(RunCaptureChecks(&CheckRecreatedReportUsesFreshFileAndPreservesOldTarget),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

void CheckDifferentTargetPreservesExistingEvidenceAndHasBoundedRetention() {
    ScopedDirectory scratch(UniqueScratchDir("bounded"));
    const fs::path first_directory = scratch.path / "first";
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(first_directory, "original"));
    const fs::path first = luminumbra::crash::CrashReportPath();
    const std::string before = ReadAll(first);
    const auto initial_count = OpenDescriptorCount();
    for (int target = 1; target < 8; ++target) {
        const fs::path directory = scratch.path / std::to_string(target);
        fs::create_directories(directory);
        const fs::path existing = directory / first.filename();
        // Exercise hard-link aliasing where the filesystem supports it;
        // other filesystems still check preservation of existing evidence.
        std::error_code link_error;
        fs::create_hard_link(first, existing, link_error);
        if (link_error) {
            std::ofstream out(existing);
            out << "pre-existing evidence\n";
            ASSERT_TRUE(out.good());
        }
        const std::string evidence_before = ReadAll(existing);
        ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(directory, "replacement"));
        EXPECT_NE(luminumbra::crash::CrashReportPath(), existing);
        EXPECT_EQ(ReadAll(existing), evidence_before);
        EXPECT_EQ(ReadAll(first), before);
        EXPECT_FALSE(fs::equivalent(first, luminumbra::crash::CrashReportPath()));
    }
    const fs::path last = luminumbra::crash::CrashReportPath();
    EXPECT_FALSE(luminumbra::crash::InstallCrashCapture(scratch.path / "exhausted", "ninth"));
    EXPECT_EQ(luminumbra::crash::CrashReportPath(), last);
    if (initial_count) {
        EXPECT_EQ(OpenDescriptorCount(), std::optional<std::size_t>(*initial_count + 7));
    }
    // Even after exhaustion, returning to a retained target consumes no slot.
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(first_directory, "reused"));
    EXPECT_EQ(luminumbra::crash::CrashReportPath(), first);
    EXPECT_EQ(ReadAll(first), before);
}

TEST(CrashReportTest, DifferentTargetPreservesExistingEvidenceAndHasBoundedRetention) {
    ASSERT_EXIT(
        RunCaptureChecks(&CheckDifferentTargetPreservesExistingEvidenceAndHasBoundedRetention),
        ::testing::ExitedWithCode(EXIT_SUCCESS),
        "");
}

#if !defined(_WIN32)

void CheckCrossThreadReinstallIsRejectedWithoutReplacingReport() {
    ScopedDirectory scratch(UniqueScratchDir("cross-thread"));
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "original_thread"));
    const fs::path report = luminumbra::crash::CrashReportPath();
    const std::string before = ReadAll(report);
    bool installed = true;
    std::thread other(
        [&] { installed = luminumbra::crash::InstallCrashCapture(scratch.path, "other_thread"); });
    other.join();
    EXPECT_FALSE(installed);
    EXPECT_EQ(luminumbra::crash::CrashReportPath(), report);
    EXPECT_EQ(ReadAll(report), before);
}

TEST(CrashReportTest, CrossThreadReinstallIsRejectedWithoutReplacingReport) {
    ASSERT_EXIT(RunCaptureChecks(&CheckCrossThreadReinstallIsRejectedWithoutReplacingReport),
                ::testing::ExitedWithCode(EXIT_SUCCESS),
                "");
}

std::string TestExecutablePath() {
#if defined(__linux__)
    std::string path(4096, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
        if (length <= 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < path.size()) {
            path.resize(static_cast<std::size_t>(length));
            return path;
        }
        path.resize(path.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return {};
    }
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    return std::string(path.data());
#elif defined(__FreeBSD__) || defined(__NetBSD__)
#if defined(__FreeBSD__)
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
#else
    int mib[] = {CTL_KERN, KERN_PROC_ARGS, static_cast<int>(::getpid()), KERN_PROC_PATHNAME};
#endif
    std::size_t size = 0;
    if (::sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::vector<char> path(size + 1, '\0');
    if (::sysctl(mib, 4, path.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    return std::string(path.data());
#else
    // Other POSIX targets can use the argv saved by gtest_main at initialization.
    const auto& arguments = ::testing::internal::GetArgvs();
    return arguments.empty() ? std::string{} : arguments.front();
#endif
}

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
    std::raise(SIGSEGV);
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
    const std::string self = TestExecutablePath();
    const auto length = self.size();
    ASSERT_GT(length, 0);

    ScopedDirectory scratch(UniqueScratchDir("child"));
    fs::create_directories(scratch.path);
    std::string child_dir_environment = std::string(kChildDirEnv) + "=" + scratch.path.string();
    std::string child_mode_environment = std::string(kChildModeEnv) + "=" + mode;
    std::vector<char*> child_environment;
#if defined(__APPLE__)
    char** environment = *_NSGetEnviron();
#else
    char** environment = ::environ;
#endif
    for (char** entry = environment; *entry != nullptr; ++entry) {
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

#else

// Synthetic filter calls exercise report formatting and its return contract without
// raising an exception in the CI runner. Actual SEH delivery and WER dump creation
// remain NEEDS-NATIVE-RUN.
void ExpectWindowsSyntheticExceptionReport() {
    ScopedDirectory scratch(UniqueScratchDir("windows-synthetic"));
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(scratch.path, "synthetic_exception"));
    const fs::path report = luminumbra::crash::CrashReportPath();
    const auto filter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(filter);
    ASSERT_NE(filter, nullptr);

    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = GetModuleHandleW(nullptr);
    ASSERT_NE(record.ExceptionAddress, nullptr);
    // An empty context ends unwinding immediately without reading synthetic stack memory.
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{&record, &context};
    EXPECT_EQ(filter(&pointers), EXCEPTION_CONTINUE_SEARCH);

    const std::string contents = ReadAll(report);
    EXPECT_EQ(FirstLine(report), kReportHeader);
    EXPECT_NE(contents.find("\nexception_code 0xc0000005\n"), std::string::npos) << contents;
    EXPECT_NE(contents.find("\nexception_address 0x"), std::string::npos) << contents;
    EXPECT_NE(contents.find("\nthread_id " + std::to_string(GetCurrentThreadId()) + "\n"),
              std::string::npos)
        << contents;
    EXPECT_NE(contents.find("\nmodule "), std::string::npos) << contents;
    EXPECT_EQ(contents.find("\nmodule unknown"), std::string::npos) << contents;
    EXPECT_NE(contents.find(" rva 0x0\n"), std::string::npos) << contents;
    EXPECT_NE(contents.find("frames\n"), std::string::npos) << contents;

    record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
    EXPECT_EQ(filter(&pointers), EXCEPTION_CONTINUE_SEARCH);
    EXPECT_EQ(ReadAll(report), contents) << "re-entry must not append another report";
    EXPECT_EQ(filter(nullptr), EXCEPTION_CONTINUE_SEARCH);
    EXPECT_EQ(ReadAll(report), contents);
}

TEST(CrashReportTest, WindowsSyntheticExceptionReportsOnceAndContinuesSearch) {
    // The filter's once-only guard belongs to the process. Keep the synthetic calls
    // in a child so they cannot consume capture for other tests or gtest repetitions.
    ASSERT_EXIT(
        {
            ExpectWindowsSyntheticExceptionReport();
            std::_Exit(::testing::Test::HasFailure() ? EXIT_FAILURE : EXIT_SUCCESS);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS),
        "");
}

#endif
