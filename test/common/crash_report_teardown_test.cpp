#include "luminumbra_common/core/CrashReport.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <system_error>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct AlternateStackLifetime {
    void* storage = nullptr;
    bool released = false;
};

AlternateStackLifetime& StackLifetime() noexcept {
    // Trivial storage remains usable by the exit callback after static destructors.
    static AlternateStackLifetime lifetime;
    return lifetime;
}

void ReleaseArray(void* storage) noexcept {
    auto& lifetime = StackLifetime();
    if (storage != nullptr && storage == lifetime.storage) {
        lifetime.released = true;
    }
    ::operator delete(storage);
}

void CrashDuringStaticDestruction() {
    const auto& lifetime = StackLifetime();
    // Freed heap pages can remain writable. Check release explicitly so allocator
    // reuse and sanitizers cannot determine whether this regression catches it.
    if (lifetime.storage == nullptr || lifetime.released) {
        std::_Exit(EXIT_FAILURE);
    }
    std::raise(SIGABRT);
    std::_Exit(EXIT_FAILURE);
}

void ExitWithCrashCapture(const std::filesystem::path& directory) {
    // Exit callbacks and static destructors run in reverse registration order.
    // Register first, so any owner created by installation has already been
    // destroyed when the callback checks the stack and raises the fatal signal.
    ASSERT_EQ(std::atexit(&CrashDuringStaticDestruction), 0);
    ASSERT_TRUE(luminumbra::crash::InstallCrashCapture(directory, "exit_time_crash"));
    stack_t alternate{};
    ASSERT_EQ(::sigaltstack(nullptr, &alternate), 0);
    ASSERT_EQ(alternate.ss_flags & SS_DISABLE, 0);
    ASSERT_NE(alternate.ss_sp, nullptr);
    ASSERT_GT(alternate.ss_size, 0u);
    StackLifetime().storage = alternate.ss_sp;
    std::exit(EXIT_SUCCESS);
}

bool WaitForChild(pid_t pid, int& status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return true;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct ScopedDirectory {
    std::filesystem::path path;

    ~ScopedDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

// Keep allocation replacement in this dedicated executable. Forwarding to scalar
// allocation preserves normal allocation semantics; observing array release makes
// premature alternate-stack destruction detectable without reading freed memory.
void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete[](void* storage) noexcept {
    ReleaseArray(storage);
}

void operator delete[](void* storage, std::size_t) noexcept {
    ReleaseArray(storage);
}

void operator delete[](void* storage, const std::nothrow_t&) noexcept {
    ReleaseArray(storage);
}

TEST(CrashReportTest, AlternateStackSurvivesStaticDestruction) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const ScopedDirectory scratch{
        std::filesystem::temp_directory_path() /
        ("luminumbra-crash-teardown-" + std::to_string(::getpid()) + "-" + std::to_string(stamp))};
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ExitWithCrashCapture(scratch.path);
        // A setup assertion returned before normal exit could be exercised.
        std::_Exit(EXIT_FAILURE);
    }
    int status = 0;
    ASSERT_TRUE(WaitForChild(pid, status)) << "child did not exit within 30 seconds";
    ASSERT_TRUE(WIFSIGNALED(status)) << "child status " << status;
    EXPECT_EQ(WTERMSIG(status), SIGABRT);

    const auto report = scratch.path / ("crash-" + std::to_string(pid) + ".txt");
    std::ifstream input(report);
    ASSERT_TRUE(input.is_open()) << "no crash report for child pid " << pid;
    // The fixed program name, header and single fault record fit within 4 KiB.
    // The child has exited, so the report cannot grow during the bounded read.
    ASSERT_LE(std::filesystem::file_size(report), 4096u);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(contents.find("luminumbra-crash-report v1\n"), 0u) << contents;
    EXPECT_NE(contents.find("\nsignal " + std::to_string(SIGABRT) + " SIGABRT\n"),
              std::string::npos)
        << contents;
    EXPECT_NE(contents.find("\nfault_address 0x"), std::string::npos) << contents;
}
