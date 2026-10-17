#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "luminumbra_common/persistence/ExclusiveRename.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// This executable links only GoogleTest. Intercept native calls in this one
// header instantiation, with no alternate definition linked from the engine.
// Each replacement happens synchronously at the requested race boundary.
namespace NativeRace {
#if defined(_WIN32)
BOOL WINAPI Inspect(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info);
BOOL WINAPI Close(HANDLE file);
BOOL WINAPI Dispose(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD bytes);
BOOL WINAPI Publish(LPCWSTR source, LPCWSTR destination, DWORD flags);
BOOL WINAPI Retain(HANDLE process,
                   HANDLE file,
                   HANDLE destination_process,
                   LPHANDLE retained,
                   DWORD access,
                   BOOL inherit,
                   DWORD options);
int OpenDescriptor(intptr_t file, int flags);
std::FILE* OpenStream(int file, const char* mode);
HANDLE WINAPI Open(LPCWSTR path,
                   DWORD access,
                   DWORD share,
                   LPSECURITY_ATTRIBUTES security,
                   DWORD disposition,
                   DWORD flags,
                   HANDLE template_file);
#define GetFileInformationByHandle NativeRace::Inspect
#define CloseHandle NativeRace::Close
#define SetFileInformationByHandle NativeRace::Dispose
#define CreateFileW NativeRace::Open
#define MoveFileExW NativeRace::Publish
#define DuplicateHandle NativeRace::Retain
#define _open_osfhandle NativeRace::OpenDescriptor
#define _fdopen NativeRace::OpenStream
#else
int Inspect(const char* path, struct stat* info);
int Describe(int file, struct stat* info);
int Unlink(int directory, const char* path, int flags);
int Restore(int from, const char* source, int to, const char* destination);
int Retain(int file, int command, int minimum);
#define lstat NativeRace::Inspect
#define fstat NativeRace::Describe
#define unlinkat NativeRace::Unlink
#define fcntl NativeRace::Retain
#endif
} // namespace NativeRace

#if !defined(_WIN32)
namespace Luminumbra::Persistence::Detail {
int InterceptRename(int from, const char* source, int to, const char* destination);
}
#define RenameWithoutReplacement InterceptRename
#endif

#include "luminumbra_common/persistence/CreatedWorldArtifacts.h"

#if defined(_WIN32)
#undef GetFileInformationByHandle
#undef CloseHandle
#undef SetFileInformationByHandle
#undef CreateFileW
#undef MoveFileExW
#undef DuplicateHandle
#undef _open_osfhandle
#undef _fdopen
#else
#undef lstat
#undef fstat
#undef unlinkat
#undef RenameWithoutReplacement
#undef fcntl
#endif

namespace NativeRace {
enum class Boundary : std::uint8_t {
    AfterValidation,
    AfterBinding,
    BeforeIdentification,
    BeforePublication,
    AfterCreation,
    IdentificationFailure,
    UnknownIdentity,
    PublicationSourceMissing,
    PublicationDenied,
    PublicationUnavailable,
    RetentionFailure,
    DescriptorFailure,
    StreamFailure,
    RemovalDenied,
    PublicationLinkedOccupant,
    FileCreationDenied
};
struct Replacement {
    std::filesystem::path target;
    std::filesystem::path moved;
    std::filesystem::path replacement;
    Boundary boundary = Boundary::AfterValidation;
    unsigned inspections = 0;
    bool fired = false;
    std::filesystem::path restore_occupant;
    bool restore_blocked = false;
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION identity{};
    HANDLE pending_close = INVALID_HANDLE_VALUE;
#endif
};
void Replace(Replacement& race) {
    race.fired = true;
    // POSIX has already moved the original into quarantine at AfterBinding.
    // Windows keeps a share-delete handle to the original until disposition.
    if (std::filesystem::exists(race.target))
        std::filesystem::rename(race.target, race.moved);
    std::filesystem::rename(race.replacement, race.target);
}
Replacement*& Active() {
    static Replacement* replacement = nullptr;
    return replacement;
}

// Observe real native handles through the existing API interception. No handle
// is owned by this capture, so it cannot itself prevent filesystem ID reuse.
class HandleCapture {
public:
    HandleCapture() {
        current = this;
    }
    ~HandleCapture() {
        current = nullptr;
    }
    HandleCapture(const HandleCapture&) = delete;
    HandleCapture& operator=(const HandleCapture&) = delete;
    static void Observe(Luminumbra::Persistence::CreatedWorldArtifacts::NativeFile file) {
        if (!current)
            return;
        // The fixture has at most nine journal entries and a few inspection
        // handles. Bound observation storage independently of implementation.
        if (current->m_handles.size() == 128)
            throw std::runtime_error("Handle observation fixture capacity exceeded.");
        if (std::find(current->m_handles.begin(), current->m_handles.end(), file) ==
            current->m_handles.end())
            current->m_handles.push_back(file);
    }
    std::size_t live() const {
        return static_cast<std::size_t>(
            std::count_if(m_handles.begin(), m_handles.end(), [](auto file) {
#if defined(_WIN32)
                BY_HANDLE_FILE_INFORMATION info{};
                return ::GetFileInformationByHandle(file, &info) != FALSE;
#else
                struct stat info {};
                return ::fstat(file, &info) == 0;
#endif
            }));
    }

private:
    static inline const HandleCapture* current = nullptr;
    // Observation through native callbacks does not change capture ownership.
    mutable std::vector<Luminumbra::Persistence::CreatedWorldArtifacts::NativeFile> m_handles;
};

bool Fail(Boundary boundary) {
    auto* active = Active();
    if (!active || active->fired || active->boundary != boundary)
        return false;
    active->fired = true;
    return true;
}

bool BeforePublish(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto* active = Active();
    if (!active || active->fired || active->target != destination)
        return true;
    if (active->boundary == Boundary::BeforePublication)
        Replace(*active);
    if (active->boundary == Boundary::PublicationLinkedOccupant) {
        active->fired = true;
        std::filesystem::create_hard_link(source, destination);
    }
    if (active->boundary == Boundary::PublicationSourceMissing) {
        active->fired = true;
        std::filesystem::rename(source, active->moved);
    }
    if (active->boundary == Boundary::PublicationDenied ||
        active->boundary == Boundary::PublicationUnavailable) {
        active->fired = true;
#if defined(_WIN32)
        ::SetLastError(active->boundary == Boundary::PublicationDenied ? ERROR_ACCESS_DENIED
                                                                       : ERROR_NOT_SUPPORTED);
#else
        errno = active->boundary == Boundary::PublicationDenied ? EACCES : ENOSYS;
#endif
        return false;
    }
    return true;
}

#if defined(_WIN32)
BOOL WINAPI Retain(HANDLE process,
                   HANDLE file,
                   HANDLE destination_process,
                   LPHANDLE retained,
                   DWORD access,
                   BOOL inherit,
                   DWORD options) {
    if (Fail(Boundary::RetentionFailure)) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const auto result =
        ::DuplicateHandle(process, file, destination_process, retained, access, inherit, options);
    if (result)
        HandleCapture::Observe(*retained);
    return result;
}

int OpenDescriptor(intptr_t file, int flags) {
    if (Fail(Boundary::DescriptorFailure)) {
        errno = EMFILE;
        return -1;
    }
    return ::_open_osfhandle(file, flags);
}

std::FILE* OpenStream(int file, const char* mode) {
    if (Fail(Boundary::StreamFailure)) {
        errno = EMFILE;
        return nullptr;
    }
    return ::_fdopen(file, mode);
}

BOOL WINAPI Publish(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    if (!BeforePublish(source, destination))
        return FALSE;
    return ::MoveFileExW(source, destination, flags);
}

bool Matches(const BY_HANDLE_FILE_INFORMATION& info) {
    auto* active = Active();
    return active && info.dwVolumeSerialNumber == active->identity.dwVolumeSerialNumber &&
           info.nFileIndexHigh == active->identity.nFileIndexHigh &&
           info.nFileIndexLow == active->identity.nFileIndexLow;
}

BOOL WINAPI Inspect(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info) {
    HandleCapture::Observe(file);
    auto* active = Active();
    if (active && !active->fired && active->boundary == Boundary::IdentificationFailure) {
        active->fired = true;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const auto result = ::GetFileInformationByHandle(file, info);
    if (result && active && !active->fired && active->boundary == Boundary::AfterCreation) {
        active->fired = true;
        std::filesystem::rename(active->target, active->moved);
    }
    if (result && active && !active->fired && active->boundary == Boundary::UnknownIdentity) {
        active->fired = true;
        info->nFileIndexHigh = 0;
        info->nFileIndexLow = 0;
    }
    if (result && active && !active->fired && Matches(*info) &&
        active->boundary == Boundary::AfterValidation && ++active->inspections == 2)
        active->pending_close = file;
    return result;
}

HANDLE WINAPI Open(LPCWSTR path,
                   DWORD access,
                   DWORD share,
                   LPSECURITY_ATTRIBUTES security,
                   DWORD disposition,
                   DWORD flags,
                   HANDLE template_file) {
    auto* active = Active();
    if (active && active->target == path && disposition == CREATE_NEW &&
        Fail(Boundary::FileCreationDenied)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (active && !active->fired && active->boundary == Boundary::BeforeIdentification &&
        active->target == path)
        Replace(*active);
    if (active && !active->fired && active->boundary == Boundary::BeforePublication &&
        (access & FILE_ADD_SUBDIRECTORY) != 0 && active->target.parent_path() == path)
        Replace(*active);
    return ::CreateFileW(path, access, share, security, disposition, flags, template_file);
}

BOOL WINAPI Close(HANDLE file) {
    auto* active = Active();
    const auto result = ::CloseHandle(file);
    // Swap only after the validation handle closes, so the old implementation
    // reaches its pathname removal with a different, fully closed entry.
    if (result && active && !active->fired && active->pending_close == file)
        Replace(*active);
    return result;
}

BOOL WINAPI Dispose(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD bytes) {
    auto* active = Active();
    BY_HANDLE_FILE_INFORMATION identity{};
    if (::GetFileInformationByHandle(file, &identity) &&
        (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        Fail(Boundary::RemovalDenied)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (active && !active->fired && active->boundary == Boundary::AfterBinding &&
        ::GetFileInformationByHandle(file, &identity) && Matches(identity))
        Replace(*active);
    return ::SetFileInformationByHandle(file, kind, info, bytes);
}
#else
int Retain(int file, int command, int minimum) {
    if (Fail(Boundary::RetentionFailure)) {
        errno = EMFILE;
        return -1;
    }
    const auto retained = ::fcntl(file, command, minimum);
    if (retained >= 0)
        HandleCapture::Observe(retained);
    return retained;
}

int Inspect(const char* path, struct stat* info) {
    auto* active = Active();
    if (active && !active->fired && active->target == path &&
        active->boundary == Boundary::BeforeIdentification)
        Replace(*active);
    const int result = ::lstat(path, info);
    if (result == 0 && active && !active->fired && active->target == path &&
        active->boundary == Boundary::AfterValidation && ++active->inspections == 2)
        Replace(*active);
    return result;
}

int Describe(int file, struct stat* info) {
    HandleCapture::Observe(file);
    auto* active = Active();
    if (active && !active->fired && active->boundary == Boundary::IdentificationFailure) {
        active->fired = true;
        errno = EIO;
        return -1;
    }
    const auto result = ::fstat(file, info);
    if (result == 0 && active && !active->fired && active->boundary == Boundary::UnknownIdentity) {
        active->fired = true;
        info->st_ino = 0;
    }
    return result;
}

int Unlink(int directory, const char* path, int flags) {
    if (flags == AT_REMOVEDIR && std::string(path) == "artifact" && Fail(Boundary::RemovalDenied)) {
        errno = EACCES;
        return -1;
    }
    auto* active = Active();
    if (active && !active->fired && active->boundary == Boundary::AfterBinding &&
        std::string(path) == "artifact")
        Replace(*active);
    return ::unlinkat(directory, path, flags);
}

int Restore(int from, const char* source, int to, const char* destination) {
    auto* active = Active();
    if (!BeforePublish(source, destination))
        return -1;
    if (active && !active->restore_occupant.empty() && !active->restore_blocked) {
        active->restore_blocked = true;
        std::filesystem::rename(active->restore_occupant, active->target);
    }
    const auto result =
        Luminumbra::Persistence::Detail::RenameWithoutReplacement(from, source, to, destination);
    if (result == 0 && active && !active->fired && active->boundary == Boundary::AfterCreation &&
        active->target == destination) {
        active->fired = true;
        std::filesystem::rename(active->target, active->moved);
    }
    return result;
}
#endif
} // namespace NativeRace

#if !defined(_WIN32)
namespace Luminumbra::Persistence::Detail {
int InterceptRename(int from, const char* source, int to, const char* destination) {
    return NativeRace::Restore(from, source, to, destination);
}
} // namespace Luminumbra::Persistence::Detail
#endif

namespace {
using Luminumbra::Persistence::CreatedWorldArtifacts;

class CreatedWorldCleanup : public testing::Test {
protected:
    void SetUp() override {
        std::random_device entropy;
        for (;;) {
            auto candidate =
                std::filesystem::temp_directory_path() /
                ("world-cleanup-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate)) {
                m_root = std::filesystem::canonical(candidate);
                break;
            }
        }
    }
    void TearDown() override {
        NativeRace::Active() = nullptr;
        std::error_code error;
        std::filesystem::remove_all(Root(), error);
        EXPECT_FALSE(error) << error.message();
    }
    static void Write(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        ASSERT_TRUE(output.good());
    }
    static std::string Read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    void Arm(const std::filesystem::path& target, NativeRace::Boundary boundary) {
        Race().target = target;
        Race().moved = Root() / "original";
        Race().replacement = Root() / "replacement";
        Race().boundary = boundary;
#if defined(_WIN32)
        // Read identity through a handle, independent of path spelling/8.3 aliases.
        if (boundary == NativeRace::Boundary::AfterValidation ||
            boundary == NativeRace::Boundary::AfterBinding) {
            const auto file =
                ::CreateFileW(target.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
            ASSERT_NE(file, INVALID_HANDLE_VALUE);
            const auto identified = ::GetFileInformationByHandle(file, &Race().identity);
            EXPECT_TRUE(::CloseHandle(file));
            ASSERT_TRUE(identified);
        }
#endif
        NativeRace::Active() = &Race();
    }
    const std::filesystem::path& Root() const {
        return m_root;
    }
    NativeRace::Replacement& Race() {
        return m_race;
    }

private:
    std::filesystem::path m_root{};
    NativeRace::Replacement m_race{};
};

TEST_F(CreatedWorldCleanup, CreatingHandlesRemainOpenUntilReleaseOrDestruction) {
    const NativeRace::HandleCapture handles;
    const auto save = Root() / "created-world";
    {
        CreatedWorldArtifacts artifacts;
        artifacts.create_root(save);
        EXPECT_GE(handles.live(), 1u);
        artifacts.create_directory(save / "chunks");
        EXPECT_GE(handles.live(), 2u);
        artifacts.write_file(save / "preset.json", "owned bytes\n");
        EXPECT_GE(handles.live(), 3u); // The writer has already closed.
        artifacts.release();
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_TRUE(std::filesystem::is_directory(save / "chunks"));
        EXPECT_EQ(Read(save / "preset.json"), "owned bytes\n");
        artifacts.release();
        EXPECT_TRUE(artifacts.rollback().empty());
        artifacts.create_root(Root() / "another-world");
        EXPECT_GE(handles.live(), 1u);
    }
    EXPECT_EQ(handles.live(), 0u);
    EXPECT_TRUE(std::filesystem::is_directory(Root() / "another-world"));
}

TEST_F(CreatedWorldCleanup, RollbackRetainsHandlesOnRefusalAndClosesThemAfterRemoval) {
    const NativeRace::HandleCapture handles;
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    artifacts.create_directory(save / "chunks");
    artifacts.write_file(save / "chunks/preset.json", "");
    Write(save / "foreign.bin", "foreign bytes\n");
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_GE(handles.live(), 3u);
    EXPECT_EQ(Read(save / "foreign.bin"), "foreign bytes\n");
    ASSERT_TRUE(std::filesystem::remove(save / "foreign.bin"));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_EQ(handles.live(), 0u);
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(CreatedWorldCleanup, UnlinkedCreationStaysPinnedAndItsReplacementSurvivesRollback) {
    for (const bool directory : {false, true}) {
        SCOPED_TRACE(directory);
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / (directory ? "directory-world" : "file-world");
        const auto artifact = save / "artifact";
        const auto moved = Root() / "unlinked-original";
        artifacts.create_root(save);
        if (directory)
            artifacts.create_directory(artifact);
        else
            artifacts.write_file(artifact, "owned bytes\n");
        std::filesystem::rename(artifact, moved);
        ASSERT_TRUE(std::filesystem::remove(moved));
        EXPECT_GE(handles.live(), 2u); // An unlink cannot free the owned ID.
        if (directory) {
            ASSERT_TRUE(std::filesystem::create_directory(artifact));
        } else {
            Write(artifact, "replacement bytes\n");
        }
        EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
        EXPECT_GE(handles.live(), 2u);
        artifacts.release();
        EXPECT_EQ(handles.live(), 0u);
        if (directory) {
            EXPECT_TRUE(std::filesystem::is_directory(artifact));
        } else {
            EXPECT_EQ(Read(artifact), "replacement bytes\n");
        }
    }
}

TEST_F(CreatedWorldCleanup, PublicationKeepsBothNamesPinnedThroughRollbackInEitherOrder) {
    for (const bool destination_first : {false, true}) {
        SCOPED_TRACE(destination_first);
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.create_root(save);
        if (destination_first)
            artifacts.reserve_file(destination);
        artifacts.write_file(temp, "owned bytes\n");
        if (!destination_first)
            artifacts.reserve_file(destination);
        artifacts.publish_file(temp, destination);
        EXPECT_GE(handles.live(), 3u);
        EXPECT_EQ(Read(destination), "owned bytes\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_FALSE(std::filesystem::exists(save));
    }
}

TEST_F(CreatedWorldCleanup, FileRetentionAndIdentificationFailuresRefuseUncertainCleanup) {
    for (const auto boundary : {NativeRace::Boundary::RetentionFailure,
                                NativeRace::Boundary::IdentificationFailure,
                                NativeRace::Boundary::UnknownIdentity}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / std::to_string(static_cast<unsigned>(boundary));
        const auto file = save / "preset.json";
        artifacts.create_root(save);
        Arm(file, boundary);
        EXPECT_THROW(artifacts.write_file(file, "owned bytes\n"), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_TRUE(std::filesystem::is_regular_file(file));
        EXPECT_TRUE(Read(file).empty());
        EXPECT_FALSE(artifacts.rollback().empty());
        EXPECT_GE(handles.live(), boundary == NativeRace::Boundary::RetentionFailure ? 1u : 2u);
        artifacts.release();
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_TRUE(std::filesystem::is_regular_file(file));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

TEST_F(CreatedWorldCleanup, PublicationRetentionFailurePreservesSourceAndVacantDestination) {
    const NativeRace::HandleCapture handles;
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::RetentionFailure);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_GE(handles.live(), 2u);
    EXPECT_EQ(Read(temp), "owned bytes\n");
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_EQ(handles.live(), 0u);
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(CreatedWorldCleanup, PartialRollbackCannotReclaimARemovedIdentityOnRetry) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto file = save / "preset.json";
    const auto alias = Root() / "other-owner";
    artifacts.create_root(save);
    artifacts.write_file(file, "shared bytes\n");
    std::filesystem::create_hard_link(file, alias);
    Arm(save, NativeRace::Boundary::RemovalDenied);
    EXPECT_FALSE(artifacts.rollback().empty());
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(file));
    ASSERT_TRUE(std::filesystem::is_directory(save));
    // This new directory entry deliberately has exactly the removed object's
    // identity, without depending on a filesystem's inode allocation policy.
    std::filesystem::create_hard_link(alias, file);
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(Read(file), "shared bytes\n");
    EXPECT_EQ(Read(alias), "shared bytes\n");
    artifacts.release();
}

TEST_F(CreatedWorldCleanup, RefusedPublicationNeverClaimsARacingHardLinkToTheSource) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationLinkedOccupant);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(Read(temp), "owned bytes\n");
    EXPECT_EQ(Read(destination), "owned bytes\n");
    ASSERT_TRUE(std::filesystem::remove(destination));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

#if defined(_WIN32)
TEST_F(CreatedWorldCleanup, FailedExclusiveCreationAbandonsOnlyItsVacantReservation) {
    const NativeRace::HandleCapture handles;
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto file = save / "preset.json";
    artifacts.create_root(save);
    Arm(file, NativeRace::Boundary::FileCreationDenied);
    EXPECT_THROW(artifacts.write_file(file, "owned bytes\n"), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(file));
    EXPECT_EQ(handles.live(), 1u);
    artifacts.write_file(file, "retry bytes\n");
    EXPECT_EQ(Read(file), "retry bytes\n");
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_EQ(handles.live(), 0u);
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(CreatedWorldCleanup, StreamBindingFailureKeepsOwnedFileAvailableForRollback) {
    for (const auto boundary :
         {NativeRace::Boundary::DescriptorFailure, NativeRace::Boundary::StreamFailure}) {
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto file = save / "preset.json";
        artifacts.create_root(save);
        Arm(file, boundary);
        EXPECT_THROW(artifacts.write_file(file, "owned bytes\n"), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_GE(handles.live(), 2u);
        EXPECT_TRUE(Read(file).empty());
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}
#endif

TEST_F(CreatedWorldCleanup, ExclusiveFilePublicationMovesBytesAndRetainsRollbackOwnership) {
    for (const auto* bytes : {"owned bytes\n", ""}) {
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        artifacts.create_root(save);
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.write_file(temp, bytes);
        artifacts.reserve_file(destination);
        artifacts.publish_file(temp, destination);
        EXPECT_FALSE(std::filesystem::exists(temp));
        EXPECT_EQ(Read(destination), bytes);
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(save));
    }
}

TEST_F(CreatedWorldCleanup, FileAppearingAfterPublicationValidationIsNeverOverwritten) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Write(Root() / "replacement", "unowned bytes\n");
    Arm(destination, NativeRace::Boundary::BeforePublication);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_EQ(Read(destination), "unowned bytes\n");
    EXPECT_EQ(Read(temp), "owned bytes\n");
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_EQ(Read(destination), "unowned bytes\n");
    EXPECT_EQ(Read(temp), "owned bytes\n");
    // Once the other owner withdraws its file, only our original stage remains.
    ASSERT_TRUE(std::filesystem::remove(destination));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(CreatedWorldCleanup, MissingPublicationSourceCannotClaimAnUncreatedDestination) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationSourceMissing);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
}

TEST_F(CreatedWorldCleanup, FailedExclusivePublicationRetainsTheStageForRollback) {
    for (const auto boundary :
         {NativeRace::Boundary::PublicationDenied, NativeRace::Boundary::PublicationUnavailable}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        artifacts.create_root(save);
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.write_file(temp, "partial bytes\n");
        artifacts.reserve_file(destination);
        Arm(destination, boundary);
        EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_EQ(Read(temp), "partial bytes\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

TEST_F(CreatedWorldCleanup, FileReplacedAfterValidationIsRestoredAndNeverDeleted) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto file = save / "preset.json";
    artifacts.write_file(file, "owned bytes\n");
    Write(Root() / "replacement", "replacement bytes\n");
    Arm(file, NativeRace::Boundary::AfterValidation);
    const auto error = artifacts.rollback();
    EXPECT_TRUE(Race().fired);
    EXPECT_NE(error.find("changed identity"), std::string::npos) << error;
    EXPECT_EQ(Read(file), "replacement bytes\n");
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(save),
                            std::filesystem::directory_iterator()),
              1);
}

TEST_F(CreatedWorldCleanup, DirectoryReplacedAfterValidationIsRestoredAndNeverDeleted) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto directory = save / "chunks";
    artifacts.create_directory(directory);
    std::filesystem::create_directory(Root() / "replacement");
    Arm(directory, NativeRace::Boundary::AfterValidation);
    const auto error = artifacts.rollback();
    EXPECT_TRUE(Race().fired);
    EXPECT_NE(error.find("changed identity"), std::string::npos) << error;
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(save),
                            std::filesystem::directory_iterator()),
              1);
}

TEST_F(CreatedWorldCleanup, RootReplacedAfterValidationIsRestoredAndNeverDeleted) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    std::filesystem::create_directory(Root() / "replacement");
    Arm(save, NativeRace::Boundary::AfterValidation);
    const auto error = artifacts.rollback();
    EXPECT_TRUE(Race().fired);
    EXPECT_NE(error.find("changed identity"), std::string::npos) << error;
    EXPECT_TRUE(std::filesystem::is_directory(save));
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
}

TEST_F(CreatedWorldCleanup, RootReplacedBeforeIdentificationCannotBeClaimedForRollback) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    std::filesystem::create_directory(Root() / "replacement");
    Arm(save, NativeRace::Boundary::BeforeIdentification);
    EXPECT_THROW(artifacts.create_root(save), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    const auto error = artifacts.rollback();
    EXPECT_NE(error.find("changed identity"), std::string::npos) << error;
    EXPECT_TRUE(std::filesystem::is_directory(save));
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
}

TEST_F(CreatedWorldCleanup, ChildReplacedBeforeIdentificationCannotBeClaimedForRollback) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto directory = save / "chunks";
    std::filesystem::create_directory(Root() / "replacement");
    Arm(directory, NativeRace::Boundary::BeforeIdentification);
    EXPECT_THROW(artifacts.create_directory(directory), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    const auto error = artifacts.rollback();
    EXPECT_NE(error.find("changed identity"), std::string::npos) << error;
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
}

TEST_F(CreatedWorldCleanup, MissingOrUnknownCreationIdentityRefusesRollback) {
    for (const auto boundary :
         {NativeRace::Boundary::IdentificationFailure, NativeRace::Boundary::UnknownIdentity}) {
        CreatedWorldArtifacts artifacts;
        const auto save =
            Root() / ("created-world-" + std::to_string(static_cast<unsigned>(boundary)));
        Arm(save, boundary);
        EXPECT_THROW(artifacts.create_root(save), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        const auto before = std::distance(std::filesystem::directory_iterator(Root()),
                                          std::filesystem::directory_iterator());
        ASSERT_GT(before, 0);
        EXPECT_FALSE(artifacts.rollback().empty());
        // Preserve all uncertain directories, including an unpublished stage.
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(Root()),
                                std::filesystem::directory_iterator()),
                  before);
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

TEST_F(CreatedWorldCleanup, DisappearingCreatedDirectoryCannotReportSuccessfulCreation) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    Arm(save, NativeRace::Boundary::AfterCreation);
    EXPECT_THROW(artifacts.create_root(save), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(save));
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
}

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
TEST_F(CreatedWorldCleanup, OccupiedPublicationRetainsReplacementAndRollsBackOnlyTheStage) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    std::filesystem::create_directory(Root() / "replacement");
    Arm(save, NativeRace::Boundary::BeforePublication);
    EXPECT_THROW(artifacts.create_root(save), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_TRUE(std::filesystem::is_directory(save));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(Root()),
                            std::filesystem::directory_iterator()),
              1);
}

TEST_F(CreatedWorldCleanup, OccupiedChildPublicationRefusesToRemoveTheUnownedOccupant) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto directory = save / "chunks";
    std::filesystem::create_directory(Root() / "replacement");
    Arm(directory, NativeRace::Boundary::BeforePublication);
    EXPECT_THROW(artifacts.create_directory(directory), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_TRUE(std::filesystem::is_directory(directory));
#if defined(_WIN32)
    // Exclusive native creation failed before adding any staged directory.
    constexpr auto expected_entries = 1;
#else
    constexpr auto expected_entries = 2;
#endif
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(save),
                            std::filesystem::directory_iterator()),
              expected_entries);
}
#endif

TEST_F(CreatedWorldCleanup, FileAppearingAfterRemovalBindsCannotBeDeletedThroughItsOldName) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto file = save / "preset.json";
    artifacts.write_file(file, "owned bytes\n");
    Write(Root() / "replacement", "replacement bytes\n");
    Arm(file, NativeRace::Boundary::AfterBinding);
    EXPECT_FALSE(artifacts.rollback().empty()); // The save still contains the replacement.
    EXPECT_TRUE(Race().fired);
    EXPECT_EQ(Read(file), "replacement bytes\n");
    EXPECT_FALSE(std::filesystem::exists(Race().moved));
}

TEST_F(CreatedWorldCleanup, DirectoryAppearingAfterRemovalBindsCannotBeDeletedThroughItsOldName) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto directory = save / "chunks";
    artifacts.create_directory(directory);
    std::filesystem::create_directory(Root() / "replacement");
    Arm(directory, NativeRace::Boundary::AfterBinding);
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_TRUE(Race().fired);
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    EXPECT_FALSE(std::filesystem::exists(Race().moved));
}

#if defined(__linux__) || defined(__APPLE__)
TEST_F(CreatedWorldCleanup, RestoringAMismatchCannotOverwriteAnotherConcurrentOccupant) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    artifacts.create_root(save);
    const auto file = save / "preset.json";
    artifacts.write_file(file, "owned bytes\n");
    Write(Root() / "replacement", "first replacement\n");
    Race().restore_occupant = Root() / "second-replacement";
    Write(Race().restore_occupant, "second replacement\n");
    Arm(file, NativeRace::Boundary::AfterValidation);
    const auto error = artifacts.rollback();
    EXPECT_TRUE(Race().fired);
    EXPECT_TRUE(Race().restore_blocked);
    EXPECT_NE(error.find("retained in quarantine"), std::string::npos) << error;
    EXPECT_EQ(Read(file), "second replacement\n");
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
    unsigned quarantines = 0;
    for (const auto& entry : std::filesystem::directory_iterator(save)) {
        if (entry.path().filename().string().starts_with(".world-rollback-")) {
            ++quarantines;
            EXPECT_EQ(Read(entry.path() / "artifact"), "first replacement\n");
        }
    }
    EXPECT_EQ(quarantines, 1u);
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_EQ(Read(file), "second replacement\n");
}
#endif
} // namespace
