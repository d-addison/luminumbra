#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
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

#include "luminumbra_common/core/FilesystemPath.h"
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
#if defined(__APPLE__)
#include <sys/param.h>
#endif
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
HANDLE WINAPI Retain(HANDLE file, DWORD access, DWORD share, DWORD flags);
HANDLE WINAPI RetainById(HANDLE hint,
                         LPFILE_ID_DESCRIPTOR descriptor,
                         DWORD access,
                         DWORD share,
                         LPSECURITY_ATTRIBUTES security,
                         DWORD flags);
BOOL WINAPI InspectId(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD bytes);
DWORD WINAPI ResolveName(HANDLE file, LPWSTR name, DWORD size, DWORD flags);
BOOL WINAPI RetainDuplicate(HANDLE process,
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
#define ReOpenFile NativeRace::Retain
#define OpenFileById NativeRace::RetainById
#define GetFileInformationByHandleEx NativeRace::InspectId
#define GetFinalPathNameByHandleW NativeRace::ResolveName
#define DuplicateHandle NativeRace::RetainDuplicate
#define _open_osfhandle NativeRace::OpenDescriptor
#define _fdopen NativeRace::OpenStream
#else
int Inspect(const char* path, struct stat* info);
int Describe(int file, struct stat* info);
int Unlink(int directory, const char* path, int flags);
int Restore(int from, const char* source, int to, const char* destination);
int Stage(int from, const char* source, int to, const char* destination);
int DescribeAt(int directory, const char* name, struct stat* info, int flags);
int Retain(int file, int command, int minimum);
#define lstat NativeRace::Inspect
#define fstat NativeRace::Describe
#define unlinkat NativeRace::Unlink
#define fcntl NativeRace::Retain
#define renameat NativeRace::Stage
#define fstatat NativeRace::DescribeAt
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
#undef ReOpenFile
#undef OpenFileById
#undef GetFileInformationByHandleEx
#undef GetFinalPathNameByHandleW
#undef DuplicateHandle
#undef _open_osfhandle
#undef _fdopen
#else
#undef lstat
#undef fstat
#undef unlinkat
#undef RenameWithoutReplacement
#undef fcntl
#undef renameat
#undef fstatat
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
    FileCreationDenied,
    PublicationBindingDenied,
    PublicationSourceReplaced,
    RetentionIdFailure,
    RetentionUnknownId,
    PublicationParentDenied,
    PublicationNameFailure,
    PublicationNameTooLong,
    PublicationFinalNameFailure,
    PublicationStageInspectionFailure,
    PublicationStageUnknownIdentity,
    PublicationStageWrongType,
    PublicationRestorationBlocked,
    PublicationParentReplaced,
    PublicationSourceParentReplaced,
    PublicationParentMissing,
    PublicationFinalNameMismatch,
    PublicationSourceInspectionFailure
};
struct Replacement {
    std::filesystem::path target;
    std::filesystem::path moved;
    std::filesystem::path replacement;
    Boundary boundary = Boundary::AfterValidation;
    unsigned inspections = 0;
    bool fired = false;
    std::filesystem::path restore_occupant;
    std::filesystem::path publication_source;
    bool restore_blocked = false;
    bool publication_completed = false;
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

void ReplacePublicationParent() {
    auto* active = Active();
    if (!active || active->fired ||
        (active->boundary != Boundary::PublicationParentReplaced &&
         active->boundary != Boundary::PublicationSourceParentReplaced &&
         active->boundary != Boundary::PublicationParentMissing))
        return;
    active->fired = true;
    const bool source_parent = active->boundary == Boundary::PublicationSourceParentReplaced;
    const auto parent =
        source_parent ? active->publication_source.parent_path() : active->target.parent_path();
    std::filesystem::rename(parent, active->moved);
    if (active->boundary == Boundary::PublicationParentMissing)
        return;
    std::filesystem::create_directory(parent);
    if (source_parent)
        std::filesystem::create_hard_link(active->moved / active->publication_source.filename(),
                                          active->publication_source);
    std::ofstream replacement(parent / "unrelated.bin", std::ios::binary);
    replacement << "protected directory bytes\n";
    replacement.close();
    if (!replacement)
        throw std::runtime_error("Could not create publication parent replacement.");
}

bool BeforePublish(const std::filesystem::path& source, const std::filesystem::path& destination) {
    auto* active = Active();
    if (!active || active->fired)
        return true;
    if (active->target != destination) {
        // Normalized handle paths use long names and the extended prefix;
        // fixtures also exercise 8.3 ancestors. Compare the containing object,
        // while keeping the destination leaf comparison exact.
        std::error_code error;
        if (active->target.filename() != destination.filename() ||
            !std::filesystem::equivalent(
                active->target.parent_path(), destination.parent_path(), error) ||
            error)
            return true;
    }
    if (active->boundary == Boundary::BeforePublication)
        Replace(*active);
    if (active->boundary == Boundary::PublicationLinkedOccupant) {
        active->fired = true;
        std::filesystem::create_hard_link(source, destination);
        EXPECT_TRUE(std::filesystem::equivalent(source, destination));
    }
    if (active->boundary == Boundary::PublicationSourceMissing) {
        active->fired = true;
        std::filesystem::rename(source, active->moved);
    }
    if (active->boundary == Boundary::PublicationSourceReplaced) {
        active->fired = true;
        std::filesystem::rename(source, active->moved);
        std::filesystem::rename(active->replacement, source);
    }
    if (active->boundary == Boundary::PublicationRestorationBlocked) {
        active->fired = true;
        std::ofstream replacement(active->publication_source, std::ios::binary);
        replacement << "protected source bytes\n";
        replacement.close();
        if (!replacement)
            throw std::runtime_error("Could not create source restoration occupant.");
#if defined(_WIN32)
        ::SetLastError(ERROR_ACCESS_DENIED);
#else
        errno = EACCES;
#endif
        return false;
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
HANDLE WINAPI RetainById(HANDLE hint,
                         LPFILE_ID_DESCRIPTOR descriptor,
                         DWORD access,
                         DWORD share,
                         LPSECURITY_ATTRIBUTES security,
                         DWORD flags) {
    ReplacePublicationParent();
    if (Fail(Boundary::RetentionFailure)) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    const auto retained = ::OpenFileById(hint, descriptor, access, share, security, flags);
    if (retained != INVALID_HANDLE_VALUE)
        HandleCapture::Observe(retained);
    return retained;
}

BOOL WINAPI InspectId(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD bytes) {
    if (kind == FileIdInfo && Fail(Boundary::RetentionIdFailure)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const auto result = ::GetFileInformationByHandleEx(file, kind, info, bytes);
    if (result && kind == FileIdInfo && Fail(Boundary::RetentionUnknownId))
        static_cast<FILE_ID_INFO*>(info)->FileId = {};
    return result;
}

DWORD WINAPI ResolveName(HANDLE file, LPWSTR name, DWORD size, DWORD flags) {
    auto* active = Active();
    if (active && active->boundary == Boundary::PublicationFinalNameFailure &&
        ++active->inspections == 3) {
        active->fired = true;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (Fail(Boundary::PublicationNameFailure)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (Fail(Boundary::PublicationNameTooLong))
        return size;
    const auto length = ::GetFinalPathNameByHandleW(file, name, size, flags);
    if (active && !active->fired && active->boundary == Boundary::PublicationFinalNameMismatch &&
        ++active->inspections == 3 && length > 0 && length < size) {
        active->fired = true;
        name[length - 1] = L'!';
    }
    return length;
}

HANDLE WINAPI Retain(HANDLE file, DWORD access, DWORD share, DWORD flags) {
    ReplacePublicationParent();
    if (Fail(Boundary::RetentionFailure)) {
        ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    const auto result = ::ReOpenFile(file, access, share, flags);
    if (result != INVALID_HANDLE_VALUE)
        HandleCapture::Observe(result);
    return result;
}

// Keep both real retention APIs observable so reverting metadata-only retention
// still exercises the same lifetime assertions and injected failures.
BOOL WINAPI RetainDuplicate(HANDLE process,
                            HANDLE file,
                            HANDLE destination_process,
                            LPHANDLE retained,
                            DWORD access,
                            BOOL inherit,
                            DWORD options) {
    ReplacePublicationParent();
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

// Also retain the path-based interception: the hard-link regression must hit
// the real legacy API and demonstrate its same-file success when reverted.
BOOL WINAPI Publish(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    if (!BeforePublish(source, destination))
        return FALSE;
    return ::MoveFileExW(source, destination, flags);
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

bool Matches(const BY_HANDLE_FILE_INFORMATION& info) {
    auto* active = Active();
    return active && info.dwVolumeSerialNumber == active->identity.dwVolumeSerialNumber &&
           info.nFileIndexHigh == active->identity.nFileIndexHigh &&
           info.nFileIndexLow == active->identity.nFileIndexLow;
}

bool IsTarget(HANDLE file) {
    auto* active = Active();
    if (!active)
        return false;
    // Scope handle-only faults to the armed object, including long/8.3 aliases.
    // Parent validation must not consume a fault intended for a new file.
    const auto target = ::CreateFileW(active->target.c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);
    if (target == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION expected{};
    BY_HANDLE_FILE_INFORMATION actual{};
    const bool identified = ::GetFileInformationByHandle(target, &expected) &&
                            ::GetFileInformationByHandle(file, &actual);
    ::CloseHandle(target);
    return identified && actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber &&
           actual.nFileIndexHigh == expected.nFileIndexHigh &&
           actual.nFileIndexLow == expected.nFileIndexLow;
}

BOOL WINAPI Inspect(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info) {
    HandleCapture::Observe(file);
    auto* active = Active();
    const bool target = active && !active->fired && IsTarget(file);
    if (target && active->boundary == Boundary::IdentificationFailure) {
        active->fired = true;
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const auto result = ::GetFileInformationByHandle(file, info);
    if (result && target && active->boundary == Boundary::AfterCreation) {
        active->fired = true;
        std::filesystem::rename(active->target, active->moved);
    }
    if (result && target && active->boundary == Boundary::UnknownIdentity) {
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
    if (active && active->publication_completed && access == FILE_READ_ATTRIBUTES &&
        flags == FILE_FLAG_OPEN_REPARSE_POINT &&
        Fail(Boundary::PublicationSourceInspectionFailure)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (active && active->target.parent_path() == path && (share & FILE_SHARE_DELETE) == 0 &&
        Fail(Boundary::PublicationParentDenied)) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (active && !active->fired && (access & DELETE) != 0 && active->target != path) {
        // Create the alias before the DELETE handle denies delete sharing.
        // Hard-link creation opens the source too; injecting it inside the
        // rename callback would test that sharing conflict instead of an alias.
        if (active->boundary == Boundary::PublicationSourceMissing ||
            active->boundary == Boundary::PublicationSourceReplaced ||
            active->boundary == Boundary::PublicationLinkedOccupant)
            BeforePublish(path, active->target);
        if (Fail(Boundary::PublicationBindingDenied)) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
    }
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
    if (kind == FileRenameInfo) {
        // Validate the complete buffer before reading its counted string or
        // forwarding it to Win32, which also consumes the terminating null.
        constexpr auto prefix = offsetof(FILE_RENAME_INFO, FileName);
        EXPECT_GE(bytes, prefix);
        if (bytes < prefix) {
            ::SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        const auto* rename_info = static_cast<const FILE_RENAME_INFO*>(info);
        const auto capacity = (bytes - prefix) / sizeof(wchar_t);
        const auto length_in_chars = rename_info->FileNameLength / sizeof(wchar_t);
        EXPECT_EQ(rename_info->FileNameLength % sizeof(wchar_t), 0u);
        EXPECT_LT(length_in_chars, capacity);
        if (rename_info->FileNameLength % sizeof(wchar_t) != 0 || length_in_chars >= capacity) {
            ::SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        EXPECT_EQ(rename_info->FileName[length_in_chars], L'\0');
        if (rename_info->FileName[length_in_chars] != L'\0') {
            ::SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        const std::filesystem::path destination(
            std::wstring(rename_info->FileName, rename_info->FileNameLength / sizeof(wchar_t)));
        // Read the bound source name through its handle, never a guessed path.
        std::vector<wchar_t> source(32768);
        const auto length = ::GetFinalPathNameByHandleW(
            file, source.data(), static_cast<DWORD>(source.size()), FILE_NAME_NORMALIZED);
        if (length == 0 || length >= source.size())
            throw std::runtime_error("Could not inspect bound publication source.");
        if (!BeforePublish(std::filesystem::path(std::wstring(source.data(), length)), destination))
            return FALSE;
        const auto result = ::SetFileInformationByHandle(file, kind, info, bytes);
        if (active && result)
            active->publication_completed = true;
        return result;
    }
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
    ReplacePublicationParent();
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

std::filesystem::path ResolveAt(int directory, const char* name) {
    if (directory == AT_FDCWD)
        return name;
#if defined(__APPLE__)
    std::array<char, MAXPATHLEN> path{};
    if (::fcntl(directory, F_GETPATH, path.data()) != 0)
        throw std::runtime_error("Could not resolve fixture directory descriptor.");
    return std::filesystem::path(path.data()) / name;
#else
    return std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(directory)) / name;
#endif
}

int Stage(int from, const char* source, int to, const char* destination) {
    auto* active = Active();
    if (active && std::string(destination) == "artifact")
        active->publication_source = ResolveAt(from, source);
    if (active && !active->fired && std::string(destination) == "artifact" &&
        (active->boundary == Boundary::PublicationSourceMissing ||
         active->boundary == Boundary::PublicationSourceReplaced))
        BeforePublish(ResolveAt(from, source), active->target);
    return ::renameat(from, source, to, destination);
}

int DescribeAt(int directory, const char* name, struct stat* info, int flags) {
    if (Fail(Boundary::PublicationStageInspectionFailure)) {
        errno = EIO;
        return -1;
    }
    const auto result = ::fstatat(directory, name, info, flags);
    if (result == 0 && Fail(Boundary::PublicationStageUnknownIdentity))
        info->st_ino = 0;
    if (result == 0 && Fail(Boundary::PublicationStageWrongType))
        info->st_mode = S_IFDIR;
    return result;
}

int Restore(int from, const char* source, int to, const char* destination) {
    auto* active = Active();
    if (active && std::string(destination) == "artifact" && std::string(source) != "artifact") {
        active->publication_source = ResolveAt(from, source);
        if (!active->fired && (active->boundary == Boundary::PublicationSourceMissing ||
                               active->boundary == Boundary::PublicationSourceReplaced))
            BeforePublish(active->publication_source, active->target);
    }
    const auto resolved_destination = ResolveAt(to, destination);
    if (!BeforePublish(ResolveAt(from, source), resolved_destination))
        return -1;
    if (active && !active->restore_occupant.empty() && !active->restore_blocked) {
        active->restore_blocked = true;
        std::filesystem::rename(active->restore_occupant, active->target);
    }
    const auto result =
        Luminumbra::Persistence::Detail::RenameWithoutReplacement(from, source, to, destination);
    if (result == 0 && active && !active->fired && active->boundary == Boundary::AfterCreation &&
        active->target == resolved_destination) {
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

TEST_F(CreatedWorldCleanup, RetainedDescendantsAllowAncestorRenameWithoutLosingIdentity) {
    for (const bool move_root : {false, true}) {
        SCOPED_TRACE(move_root);
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto directory = save / "chunks";
        artifacts.create_root(save);
        artifacts.create_directory(directory);
        artifacts.write_file(directory / "empty.bin", "");
        const auto temp = directory / "preset.tmp";
        const auto destination = directory / "preset.json";
        artifacts.write_file(temp, "owned bytes\n");
        artifacts.reserve_file(destination);
        artifacts.publish_file(temp, destination);
        const auto original = move_root ? save : directory;
        const auto moved = Root() / "moved-creation";
        std::filesystem::rename(original, moved);
        EXPECT_GE(handles.live(), 5u);
        const auto payload = move_root ? moved / "chunks" : moved;
        EXPECT_TRUE(std::filesystem::is_regular_file(payload / "empty.bin"));
        EXPECT_EQ(Read(payload / "preset.json"), "owned bytes\n");
        ASSERT_TRUE(std::filesystem::create_directory(original));
        EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
        EXPECT_GE(handles.live(), 5u);
        ASSERT_TRUE(std::filesystem::remove(original));
        std::filesystem::rename(moved, original);
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
    ASSERT_TRUE(std::filesystem::exists(temp));
    ASSERT_TRUE(std::filesystem::exists(destination));
    ASSERT_TRUE(std::filesystem::equivalent(temp, destination));
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(Read(temp), "owned bytes\n");
    EXPECT_EQ(Read(destination), "owned bytes\n");
    ASSERT_TRUE(std::filesystem::remove(destination));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(CreatedWorldCleanup, ReplacedPublicationSourceNeverTransfersAnotherOwnersBytes) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Write(Root() / "replacement", "protected bytes\n");
    Arm(destination, NativeRace::Boundary::PublicationSourceReplaced);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::runtime_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(Read(temp), "protected bytes\n");
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    artifacts.release();
    EXPECT_EQ(Read(temp), "protected bytes\n");
    EXPECT_EQ(Read(Race().moved), "owned bytes\n");
}

TEST_F(CreatedWorldCleanup, ReplacedPublicationParentCannotReceiveOwnedBytesAndRollbackCanRetry) {
    for (const unsigned layout : {0u, 1u, 2u, 3u}) {
        SCOPED_TRACE(layout);
        const bool missing_parent = layout == 3;
        const bool nested_destination = layout == 1 || missing_parent;
        const bool replaced_source_parent = layout == 2;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        artifacts.create_root(save);
        const auto directory = save / "chunks";
        if (layout != 0)
            artifacts.create_directory(directory);
        const auto temp = (replaced_source_parent ? directory : save) / "preset.tmp";
        const auto destination = (nested_destination ? directory : save) / "preset.json";
        const auto parent = replaced_source_parent ? temp.parent_path() : destination.parent_path();
        artifacts.write_file(temp, "owned bytes\n");
        artifacts.reserve_file(destination);
        // Swap after public validation, before either native parent is bound.
        // The replacement destination stays vacant: no collision masks the race.
        Arm(destination,
            missing_parent           ? NativeRace::Boundary::PublicationParentMissing
            : replaced_source_parent ? NativeRace::Boundary::PublicationSourceParentReplaced
                                     : NativeRace::Boundary::PublicationParentReplaced);
        Race().publication_source = temp;
        EXPECT_THROW(artifacts.publish_file(temp, destination), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_EQ(Read(nested_destination ? temp : Race().moved / "preset.tmp"), "owned bytes\n");
        if (missing_parent) {
            EXPECT_FALSE(std::filesystem::exists(parent));
            EXPECT_TRUE(artifacts.rollback().empty());
            EXPECT_TRUE(std::filesystem::is_directory(Race().moved));
            EXPECT_TRUE(std::filesystem::remove(Race().moved));
        } else {
            EXPECT_EQ(Read(parent / "unrelated.bin"), "protected directory bytes\n");
            EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
            EXPECT_EQ(Read(parent / "unrelated.bin"), "protected directory bytes\n");
            if (replaced_source_parent) {
                // The same file ID never adopts a replacement parent.
                EXPECT_EQ(Read(temp), "owned bytes\n");
            }
            std::filesystem::remove_all(parent);
            std::filesystem::rename(Race().moved, parent);
            EXPECT_TRUE(artifacts.rollback().empty());
        }
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

#if !defined(_WIN32)
TEST_F(CreatedWorldCleanup, PublicationStageInspectionFailuresRestoreTheUnpublishedSource) {
    for (const auto boundary : {NativeRace::Boundary::PublicationStageInspectionFailure,
                                NativeRace::Boundary::PublicationStageUnknownIdentity,
                                NativeRace::Boundary::PublicationStageWrongType}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.create_root(save);
        artifacts.write_file(temp, "owned bytes\n");
        artifacts.reserve_file(destination);
        Arm(destination, boundary);
        EXPECT_THROW(artifacts.publish_file(temp, destination), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_EQ(Read(temp), "owned bytes\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

TEST_F(CreatedWorldCleanup, PublicationRestoreCollisionPreservesBothOwnersForRecovery) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationRestorationBlocked);
    try {
        artifacts.publish_file(temp, destination);
        FAIL() << "Publication must refuse the injected failure.";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("retained in quarantine"), std::string::npos);
    }
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_EQ(Read(temp), "protected source bytes\n");
    std::filesystem::path retained;
    for (const auto& entry : std::filesystem::directory_iterator(save))
        if (entry.is_directory()) {
            EXPECT_TRUE(retained.empty());
            retained = entry.path() / "artifact";
        }
    ASSERT_FALSE(retained.empty());
    EXPECT_EQ(Read(retained), "owned bytes\n");
    EXPECT_FALSE(artifacts.rollback().empty());
    artifacts.release();
    EXPECT_EQ(Read(temp), "protected source bytes\n");
    EXPECT_EQ(Read(retained), "owned bytes\n");
}
#endif

#if defined(_WIN32)
TEST_F(CreatedWorldCleanup, IdRetentionFailureNeverAdoptsOrDeletesAnUncertainFile) {
    for (const auto boundary :
         {NativeRace::Boundary::RetentionIdFailure, NativeRace::Boundary::RetentionUnknownId}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto file = save / "preset.json";
        artifacts.create_root(save);
        Arm(file, boundary);
        EXPECT_THROW(artifacts.write_file(file, "unwritten bytes\n"), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_TRUE(std::filesystem::is_regular_file(file));
        EXPECT_EQ(Read(file), "");
        EXPECT_FALSE(artifacts.rollback().empty());
        artifacts.release();
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_TRUE(std::filesystem::is_regular_file(file));
        NativeRace::Active() = nullptr;
        Race() = {};
        std::filesystem::remove_all(save);
    }
}

TEST_F(CreatedWorldCleanup, PublicationNameAndDirectoryFailuresPreserveBothReservationsForRetry) {
    for (const auto boundary : {NativeRace::Boundary::PublicationParentDenied,
                                NativeRace::Boundary::PublicationNameFailure,
                                NativeRace::Boundary::PublicationNameTooLong}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.create_root(save);
        artifacts.write_file(temp, "owned bytes\n");
        artifacts.reserve_file(destination);
        Arm(destination, boundary);
        EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_EQ(Read(temp), "owned bytes\n");
        EXPECT_FALSE(std::filesystem::exists(destination));
        artifacts.publish_file(temp, destination);
        EXPECT_FALSE(std::filesystem::exists(temp));
        EXPECT_EQ(Read(destination), "owned bytes\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

TEST_F(CreatedWorldCleanup, UncertainPublicationNameNeverDeletesThePublishedBytes) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationFinalNameFailure);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(temp));
    EXPECT_EQ(Read(destination), "owned bytes\n");
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(Read(destination), "owned bytes\n");
    artifacts.release();
    EXPECT_EQ(Read(destination), "owned bytes\n");
}

TEST_F(CreatedWorldCleanup, UnreadablePublicationSourceNeverClaimsTheDestination) {
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationSourceInspectionFailure);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(temp));
    EXPECT_EQ(Read(destination), "owned bytes\n");
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    artifacts.release();
    EXPECT_EQ(Read(destination), "owned bytes\n");
}

TEST_F(CreatedWorldCleanup, UnexpectedPublicationNameNeverClaimsTheDestination) {
    const NativeRace::HandleCapture handles;
    CreatedWorldArtifacts artifacts;
    const auto save = Root() / "created-world";
    const auto temp = save / "preset.tmp";
    const auto destination = save / "preset.json";
    artifacts.create_root(save);
    artifacts.write_file(temp, "owned bytes\n");
    artifacts.reserve_file(destination);
    Arm(destination, NativeRace::Boundary::PublicationFinalNameMismatch);
    EXPECT_THROW(artifacts.publish_file(temp, destination), std::system_error);
    ASSERT_TRUE(Race().fired);
    EXPECT_FALSE(std::filesystem::exists(temp));
    EXPECT_EQ(Read(destination), "owned bytes\n");
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_GE(handles.live(), 3u);
    artifacts.release();
    EXPECT_EQ(handles.live(), 0u);
    EXPECT_EQ(Read(destination), "owned bytes\n");
}

TEST_F(CreatedWorldCleanup, RefusedPublicationBindingPreservesEveryOwnersBytes) {
    for (const auto boundary : {NativeRace::Boundary::PublicationBindingDenied,
                                NativeRace::Boundary::PublicationSourceReplaced}) {
        SCOPED_TRACE(static_cast<unsigned>(boundary));
        const NativeRace::HandleCapture handles;
        CreatedWorldArtifacts artifacts;
        const auto save = Root() / "created-world";
        const auto temp = save / "preset.tmp";
        const auto destination = save / "preset.json";
        artifacts.create_root(save);
        artifacts.write_file(temp, "owned bytes\n");
        artifacts.reserve_file(destination);
        Write(Root() / "replacement", "other owner's bytes\n");
        Arm(destination, boundary);
        EXPECT_THROW(artifacts.publish_file(temp, destination), std::runtime_error);
        ASSERT_TRUE(Race().fired);
        EXPECT_GE(handles.live(), 3u);
        EXPECT_FALSE(std::filesystem::exists(destination));
        if (boundary == NativeRace::Boundary::PublicationSourceReplaced) {
            EXPECT_EQ(Read(temp), "other owner's bytes\n");
            EXPECT_EQ(Read(Race().moved), "owned bytes\n");
            EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
            ASSERT_TRUE(std::filesystem::remove(temp));
            std::filesystem::rename(Race().moved, temp);
        }
        EXPECT_EQ(Read(temp), "owned bytes\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_EQ(handles.live(), 0u);
        EXPECT_FALSE(std::filesystem::exists(save));
        NativeRace::Active() = nullptr;
        Race() = {};
    }
}

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

TEST_F(CreatedWorldCleanup, PublicationPreservesExactLeafNamesAcrossRenameBufferLengths) {
    // Exercise both UTF-16 byte-length parities modulo the native alignment,
    // short names, empty payloads and a non-ASCII leaf without heap-tail luck.
    for (const auto* leaf : {u8"a", u8"ab", u8"abc", u8"abcd", u8"世界-é.json"}) {
        for (const auto* bytes : {"exact bytes\n", ""}) {
            CreatedWorldArtifacts artifacts;
            const auto save = Root() / "created-world";
            const auto temp = save / "preset.tmp";
            const auto destination = save / std::filesystem::path(leaf);
            SCOPED_TRACE(Luminumbra::Filesystem::GenericUtf8PathString(destination));
            artifacts.create_root(save);
            artifacts.write_file(temp, bytes);
            artifacts.reserve_file(destination);
            artifacts.publish_file(temp, destination);
            EXPECT_FALSE(std::filesystem::exists(temp));
            EXPECT_EQ(Read(destination), bytes);
            ASSERT_EQ(std::distance(std::filesystem::directory_iterator(save),
                                    std::filesystem::directory_iterator()),
                      1);
            EXPECT_EQ(std::filesystem::directory_iterator(save)->path().filename(),
                      destination.filename());
            EXPECT_TRUE(artifacts.rollback().empty());
            EXPECT_FALSE(std::filesystem::exists(save));
        }
    }
}

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
