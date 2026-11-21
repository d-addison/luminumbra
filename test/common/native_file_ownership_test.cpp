#include "luminumbra_common/core/BoundedConsoleSink.h"
#include "luminumbra_common/core/CrashReport.h"
#include "luminumbra_common/core/NativeFileOwner.h"
#include "luminumbra_common/persistence/ReplacementOutputFile.h"
#include "luminumbra_common/persistence/SavedInputFile.h"
#include "support/UniqueTempPath.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
struct AllocationFault {
    bool enabled = false;
    std::size_t remaining = 0;
    bool fired = false;
    bool file_open_at_failure = false;
    bool wait_for_file = false;
    const std::filesystem::path* required_file = nullptr;
#if defined(_WIN32)
    DWORD baseline = 0;
#else
    int baseline = -1;
#endif
};

AllocationFault& Fault() noexcept {
    static thread_local AllocationFault fault;
    return fault;
}

#if defined(_WIN32)
DWORD HandleCount() noexcept {
    DWORD count = 0;
    if (!::GetProcessHandleCount(::GetCurrentProcess(), &count))
        std::abort();
    return count;
}
#else
int NextDescriptor() noexcept {
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        std::abort();
    ::close(descriptor);
    return descriptor;
}
#endif

bool FileOpenedSinceBaseline() noexcept {
#if defined(_WIN32)
    const auto saved_error = ::GetLastError();
    const bool opened = HandleCount() > Fault().baseline;
    ::SetLastError(saved_error);
#else
    const int saved_error = errno;
    const bool opened = ::fcntl(Fault().baseline, F_GETFD) >= 0;
    errno = saved_error;
#endif
    return opened;
}

bool RequiredFileExists(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    const auto saved_error = ::GetLastError();
    const bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    ::SetLastError(saved_error);
#else
    const int saved_error = errno;
    struct stat info {};
    const bool exists = ::lstat(path.c_str(), &info) == 0;
    errno = saved_error;
#endif
    return exists;
}

void FailAllocation() {
    auto& fault = Fault();
    if (!fault.enabled || (fault.wait_for_file && !FileOpenedSinceBaseline()))
        return;
    if (fault.required_file && !RequiredFileExists(*fault.required_file))
        return;
    if (fault.remaining != 0) {
        --fault.remaining;
        return;
    }
    fault.enabled = false;
    fault.fired = true;
    fault.file_open_at_failure = FileOpenedSinceBaseline();
    throw std::bad_alloc();
}

class FaultScope {
public:
    explicit FaultScope(std::size_t ordinal,
                        bool wait_for_file = false,
                        const std::filesystem::path* required_file = nullptr) {
        Fault() = {.enabled = true,
                   .remaining = ordinal,
                   .fired = false,
                   .file_open_at_failure = false,
                   .wait_for_file = wait_for_file,
                   .required_file = required_file,
#if defined(_WIN32)
                   .baseline = HandleCount()};
#else
                   .baseline = NextDescriptor()};
#endif
    }
    ~FaultScope() {
        Fault().enabled = false;
    }
};

void FreeAligned(void* storage) noexcept {
#if defined(_WIN32)
    ::_aligned_free(storage);
#else
    std::free(storage);
#endif
}
} // namespace

// Isolated executable: all allocation/deallocation forms use the same heap.
void* operator new(std::size_t size) {
    FailAllocation();
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
        return storage;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* storage) noexcept {
    std::free(storage);
}
void operator delete[](void* storage) noexcept {
    ::operator delete(storage);
}
void operator delete(void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    FailAllocation();
    void* storage = nullptr;
#if defined(_WIN32)
    storage = ::_aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
#else
    if (::posix_memalign(&storage, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) !=
        0) {
        storage = nullptr;
    }
#endif
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    return storage;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* storage, std::align_val_t) noexcept {
    FreeAligned(storage);
}
void operator delete[](void* storage, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete(void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
// The standard library also allocates through the nothrow forms (for example
// std::stable_sort's temporary buffer), so they must share the replaced heap.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}
void operator delete(void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}
void operator delete(void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}

namespace {
using Luminumbra::Persistence::SavedInputFile;

// At most 256 allocation positions per cold operation. No worker threads run
// while the fault is armed; OS probes do not allocate or open persistent files.
template<typename Operation>
void ExpectAllocationFailureClosesFiles(const Operation& operation) {
    bool saw_open_failure = false;
    for (std::size_t ordinal = 0; ordinal < 256; ++ordinal) {
        bool succeeded = false;
        {
            const FaultScope scope(ordinal);
            try {
                succeeded = operation();
            } catch (const std::bad_alloc&) {}
        }
        const auto fault = Fault();
        if (!fault.fired) {
            EXPECT_TRUE(succeeded);
            EXPECT_TRUE(saw_open_failure);
            return;
        }
        saw_open_failure = saw_open_failure || fault.file_open_at_failure;
#if defined(_WIN32)
        ASSERT_EQ(HandleCount(), fault.baseline) << "allocation " << ordinal;
#else
        // The first free descriptor must remain closed, including when the
        // allocation seam observed it open immediately before throwing.
        errno = 0;
        ASSERT_EQ(::fcntl(fault.baseline, F_GETFD), -1) << "allocation " << ordinal;
        ASSERT_EQ(errno, EBADF);
#endif
    }
    FAIL() << "operation exceeded the allocation-position bound";
}

TEST(NativeFileOwnership, RetainedIdentityClosesOnAllocationFailure) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("retained_identity", ".bin");
    const auto& path = temporary.Path();
    ASSERT_TRUE(static_cast<bool>(std::ofstream(path) << "unchanged"));
    const SavedInputFile input(path);
    ExpectAllocationFailureClosesFiles([&] { return static_cast<bool>(input.retain_identity()); });
    std::string bytes;
    ASSERT_TRUE(input.read(bytes, 9));
    EXPECT_EQ(bytes, "unchanged");
}

TEST(NativeFileOwnership, RetainedIdentityLivesUntilLastSharedOwner) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("retained_lifetime", ".bin");
    const auto& path = temporary.Path();
    ASSERT_TRUE(static_cast<bool>(std::ofstream(path) << "unchanged"));
    std::shared_ptr<const void> retained;
#if defined(_WIN32)
    const auto baseline = HandleCount();
#else
    const auto baseline = NextDescriptor();
    int duplicate = -1;
#endif
    {
        const SavedInputFile input(path);
#if !defined(_WIN32)
        duplicate = NextDescriptor();
#endif
        retained = input.retain_identity();
        ASSERT_NE(retained, nullptr);
    }
    auto other = retained;
    retained.reset();
#if defined(_WIN32)
    EXPECT_EQ(HandleCount(), baseline + 1);
#else
    EXPECT_GE(::fcntl(duplicate, F_GETFD), 0);
#endif
    other.reset();
#if defined(_WIN32)
    EXPECT_EQ(HandleCount(), baseline);
#else
    errno = 0;
    EXPECT_EQ(::fcntl(duplicate, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(NextDescriptor(), baseline);
#endif
}

TEST(NativeFileOwnership, InvalidOwnersDoNotCloseOtherFiles) {
#if defined(_WIN32)
    const auto baseline = HandleCount();
    {
        const Luminumbra::Filesystem::NativeFileOwner invalid(INVALID_HANDLE_VALUE);
        const Luminumbra::Filesystem::NativeFileOwner empty(nullptr);
    }
    EXPECT_EQ(HandleCount(), baseline);
#else
    const auto baseline = NextDescriptor();
    { const Luminumbra::Filesystem::NativeFileOwner invalid(-1); }
    EXPECT_EQ(NextDescriptor(), baseline);
#endif
}

TEST(NativeFileOwnership, ConsoleDuplicateLivesUntilLastSharedOwner) {
#if defined(_WIN32)
    const auto baseline = HandleCount();
#else
    const auto baseline = NextDescriptor();
#endif
    auto output = BoundedConsoleSink::MakeStdoutOutput();
    ASSERT_NE(output, nullptr);
    auto other = output;
    output.reset();
#if defined(_WIN32)
    EXPECT_EQ(HandleCount(), baseline + 1);
#else
    EXPECT_GE(::fcntl(baseline, F_GETFD), 0);
#endif
    other.reset();
#if defined(_WIN32)
    EXPECT_EQ(HandleCount(), baseline);
#else
    errno = 0;
    EXPECT_EQ(::fcntl(baseline, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
#endif
}

TEST(NativeFileOwnership, MissingInputCannotRetainIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("missing_identity", ".bin");
    const SavedInputFile input(temporary.Path());
    EXPECT_TRUE(input.missing());
    EXPECT_EQ(input.retain_identity(), nullptr);
}

TEST(NativeFileOwnership, SavedInputAdmissionClosesOnAllocationFailure) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("input_admission", ".bin");
    const auto& path = temporary.Path();
    ASSERT_TRUE(static_cast<bool>(std::ofstream(path) << "unchanged"));
    ExpectAllocationFailureClosesFiles([&] {
        const SavedInputFile input(path);
        std::uint64_t length = 0;
        return input.length(length) && length == 9;
    });
}

TEST(NativeFileOwnership, ConsoleDuplicateClosesOnAllocationFailure) {
    ExpectAllocationFailureClosesFiles(
        [] { return static_cast<bool>(BoundedConsoleSink::MakeStdoutOutput()); });
}

#if !defined(_WIN32)
TEST(NativeFileOwnership, InvalidConsoleDescriptorIsRefused) {
    EXPECT_EQ(BoundedConsoleSink::MakeFileDescriptorOutput(-1), nullptr);
}
#endif

TEST(NativeFileOwnership, ReplayPairLockClosesOnAllocationFailure) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("pair_lock", ".replay");
    const auto& path = temporary.Path();
    ExpectAllocationFailureClosesFiles([&] {
        const Luminumbra::Persistence::ReplayPairReadGuard guard(path);
        return guard.is_open();
    });
}

TEST(NativeFileOwnership, ReplayPairLockRefusesMissingOrNonDirectoryParent) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("pair_parent", ".bin");
    const auto nested = temporary.Path() / "input.replay";
    {
        const Luminumbra::Persistence::ReplayPairReadGuard missing(nested);
        EXPECT_FALSE(missing.is_open());
    }
    ASSERT_TRUE(static_cast<bool>(std::ofstream(temporary.Path()) << "unchanged"));
    const Luminumbra::Persistence::ReplayPairReadGuard wrong_type(nested);
    EXPECT_FALSE(wrong_type.is_open());
}

TEST(NativeFileOwnership, CrashReportClosesUnpublishedFileOnAllocationFailure) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("report_lifetime", ".txt");
    const auto directory = temporary.Path().parent_path();
    // Fault the first allocation after open, so this test never publishes a
    // process-lifetime report handle. All files can be closed before cleanup.
#if defined(_WIN32)
    const auto process = ::GetCurrentProcessId();
#else
    const auto process = ::getpid();
#endif
    const auto report = directory / ("crash-" + std::to_string(process) + ".txt");
    bool threw = false;
    {
        const FaultScope scope(0, true, &report);
        try {
            luminumbra::crash::InstallCrashCapture(directory, "lifetime_test");
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    }
    const auto fault = Fault();
    EXPECT_TRUE(threw);
    EXPECT_TRUE(fault.fired);
    EXPECT_TRUE(fault.file_open_at_failure);
#if defined(_WIN32)
    EXPECT_EQ(HandleCount(), fault.baseline);
#else
    errno = 0;
    EXPECT_EQ(::fcntl(fault.baseline, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
#endif
    EXPECT_TRUE(luminumbra::crash::CrashReportPath().empty());
}
} // namespace
