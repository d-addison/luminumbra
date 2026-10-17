#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

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
#else
int Inspect(const char* path, struct stat* info);
int Describe(int file, struct stat* info);
int Unlink(int directory, const char* path, int flags);
int Restore(int from, const char* source, int to, const char* destination);
#define lstat NativeRace::Inspect
#define fstat NativeRace::Describe
#define unlinkat NativeRace::Unlink
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
#else
#undef lstat
#undef fstat
#undef unlinkat
#undef RenameWithoutReplacement
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
    PublicationUnavailable
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

bool BeforePublish(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto* active = Active();
    if (!active || active->fired || active->target != destination)
        return true;
    if (active->boundary == Boundary::BeforePublication)
        Replace(*active);
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
    if (active && !active->fired && active->boundary == Boundary::AfterBinding &&
        ::GetFileInformationByHandle(file, &identity) && Matches(identity))
        Replace(*active);
    return ::SetFileInformationByHandle(file, kind, info, bytes);
}
#else
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
