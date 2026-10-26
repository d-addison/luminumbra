#pragma once

#include "../core/FilesystemPath.h"
#include "ExclusiveRename.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <winternl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Luminumbra::Persistence {

#if defined(_WIN32)
namespace Detail {
class WindowsPublicationHandle {
public:
    explicit WindowsPublicationHandle(HANDLE file)
        : m_file(file) {}
    ~WindowsPublicationHandle() {
        if (m_file != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_file);
    }
    WindowsPublicationHandle(const WindowsPublicationHandle&) = delete;
    WindowsPublicationHandle& operator=(const WindowsPublicationHandle&) = delete;
    HANDLE get() const {
        return m_file;
    }
    HANDLE release() {
        return std::exchange(m_file, INVALID_HANDLE_VALUE);
    }
    void reset(HANDLE file = INVALID_HANDLE_VALUE) {
        if (m_file != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_file);
        m_file = file;
    }

private:
    HANDLE m_file;
};

inline std::filesystem::path WindowsPublicationPath(HANDLE file) {
    // The Win32 extended path limit includes the terminating UTF-16 null.
    std::array<wchar_t, 32768> name{};
    const auto length = ::GetFinalPathNameByHandleW(
        file, name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
    if (length == 0 || length >= name.size())
        throw std::system_error(
            static_cast<int>(length == 0 ? ::GetLastError() : ERROR_FILENAME_EXCED_RANGE),
            std::system_category(),
            "Could not resolve bound world artifact name");
    return std::filesystem::path(std::wstring(name.data(), length));
}

inline DWORD WindowsVacantPublicationNameError(const std::filesystem::path& destination) {
    const auto occupant = ::CreateFileW(destination.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
    if (occupant != INVALID_HANDLE_VALUE) {
        ::CloseHandle(occupant);
        return ERROR_ALREADY_EXISTS;
    }
    const auto error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}

inline DWORD WindowsPublicationLinkCount(HANDLE file) {
    FILE_STANDARD_INFO info{};
    if (!::GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)))
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(),
                                "Could not inspect publication link count");
    if (info.NumberOfLinks == 0)
        throw std::system_error(
            ERROR_INVALID_DATA, std::system_category(), "Unknown publication link count");
    return info.NumberOfLinks;
}

// Recovery adds the retired source link through the bound object. Renaming back
// would consume the other owner's destination link. FileLinkInformation requires
// DELETE access, already held by the publication handle; ReplaceIfExists is false.
inline void WindowsRestorePublicationSource(HANDLE file, const std::filesystem::path& source) {
    const auto vacancy = WindowsVacantPublicationNameError(source);
    if (vacancy != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(vacancy),
                                std::system_category(),
                                "Could not inspect publication recovery name");
    struct LinkInformation {
        BOOLEAN ReplaceIfExists;
        HANDLE RootDirectory;
        ULONG FileNameLength;
        WCHAR FileName[1];
    };
    const auto& win32_name = source.native();
    // Recovery receives the source handle's normalized extended name. Check
    // before copying; replacing the four-unit prefix does not grow the string.
    if (win32_name.size() > 32767 || !win32_name.starts_with(L"\\\\?\\"))
        throw std::system_error(
            ERROR_INVALID_NAME, std::system_category(), "Invalid bound publication recovery name");
    auto name = win32_name;
    // NtSetInformationFile consumes an NT object-manager path, rather than
    // the Win32 spelling accepted by SetFileInformationByHandle.
    name.replace(0, 4, L"\\??\\");
    const auto name_bytes = name.size() * sizeof(wchar_t);
    const auto record_bytes =
        (std::max)(sizeof(LinkInformation), offsetof(LinkInformation, FileName) + name_bytes);
    std::vector<std::byte> storage(record_bytes);
    auto* link = reinterpret_cast<LinkInformation*>(storage.data());
    link->ReplaceIfExists = FALSE;
    link->RootDirectory = nullptr;
    link->FileNameLength = static_cast<ULONG>(name_bytes);
    std::memcpy(link->FileName, name.data(), name_bytes);
    using LinkFunction =
        NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    const auto library = ::GetModuleHandleW(L"ntdll.dll");
    const FARPROC entry = library ? ::GetProcAddress(library, "NtSetInformationFile") : nullptr;
    const auto set_information = std::bit_cast<LinkFunction>(entry);
    if (!set_information)
        throw std::system_error(ERROR_NOT_SUPPORTED,
                                std::system_category(),
                                "Publication source recovery is unavailable");
    IO_STATUS_BLOCK status{};
    // Native FILE_INFORMATION_CLASS FileLinkInformation (11).
    if (set_information(file,
                        &status,
                        link,
                        static_cast<ULONG>(record_bytes),
                        static_cast<FILE_INFORMATION_CLASS>(11)) < 0)
        throw std::system_error(ERROR_CANNOT_MAKE,
                                std::system_category(),
                                "Could not restore publication source link; destination retained");
}

// A failed recovery may leave a link created by this operation. Only a verified
// two-link state can enter the journal for a later identity-checked rollback.
class WindowsPublicationRecoveryError : public std::system_error {
public:
    WindowsPublicationRecoveryError(const std::string& message, bool verified_link)
        : std::system_error(ERROR_CANNOT_MAKE, std::system_category(), message)
        , m_verified_link(verified_link) {}
    bool verified_link() const {
        return m_verified_link;
    }

private:
    bool m_verified_link;
};

// Takes ownership of the source handle, opened with DELETE access and without
// FILE_SHARE_DELETE. Parents and the source name remain bound while the hard
// link is created. CreateHardLinkW atomically refuses ANY occupied destination,
// including another link to this file; a vacancy probe is only an early refusal.
// Unsupported filesystems (including FAT/exFAT and some network shares) fail
// closed with the native error. There is deliberately no rename fallback.
inline HANDLE WindowsPublishWithoutReplacement(HANDLE file,
                                               const std::filesystem::path& source,
                                               const std::filesystem::path& destination,
                                               DWORD access) {
    WindowsPublicationHandle original(file);
    const WindowsPublicationHandle parent(
        ::CreateFileW(destination.parent_path().empty() ? L"." : destination.parent_path().c_str(),
                      FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                      nullptr));
    if (parent.get() == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(),
                                "Could not bind publication destination parent");
    const WindowsPublicationHandle source_parent(
        ::CreateFileW(source.parent_path().empty() ? L"." : source.parent_path().c_str(),
                      FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                      nullptr));
    if (source_parent.get() == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(),
                                "Could not bind publication parents");
    auto bound_destination = WindowsPublicationPath(parent.get()) / destination.filename();
    bound_destination.make_preferred();
    const auto original_name = WindowsPublicationPath(original.get());
    // Both Win32 names are bounded before passing them to the link operation.
    if (bound_destination.native().size() > 32767)
        throw std::system_error(ERROR_FILENAME_EXCED_RANGE,
                                std::system_category(),
                                "World artifact destination is too long");
    const auto vacancy = WindowsVacantPublicationNameError(bound_destination);
    if (vacancy != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(vacancy),
                                std::system_category(),
                                "Refusing an occupied or unreadable publication name");
    if (WindowsPublicationLinkCount(original.get()) != 1)
        throw std::system_error(
            ERROR_ALREADY_EXISTS, std::system_category(), "Refusing an aliased publication source");
    if (!::CreateHardLinkW(bound_destination.c_str(), original_name.c_str(), nullptr))
        throw std::system_error(
            static_cast<int>(::GetLastError()),
            std::system_category(),
            "Exclusive world publication requires supported same-volume hard links");

    // The source's denied delete sharing protects the newly created link until
    // it is opened. This handle names that link, rather than the source link.
    // It cannot request DELETE while the original denies delete sharing.
    WindowsPublicationHandle linked(
        ::CreateFileW(bound_destination.c_str(),
                      access & ~static_cast<DWORD>(DELETE),
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT,
                      nullptr));
    const auto binding_error =
        linked.get() == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    bool source_retired = false;
    bool linked_deletable = false;
    bool verified_link = false;
    try {
        if (WindowsPublicationLinkCount(original.get()) != 2)
            throw std::system_error(ERROR_INVALID_DATA,
                                    std::system_category(),
                                    "Could not verify created publication link count");
        verified_link = true;
        if (linked.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(binding_error),
                                    std::system_category(),
                                    "Could not bind created publication link");
        BY_HANDLE_FILE_INFORMATION retained_info{};
        BY_HANDLE_FILE_INFORMATION published_info{};
        if (!::GetFileInformationByHandle(original.get(), &retained_info) ||
            !::GetFileInformationByHandle(linked.get(), &published_info))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not inspect published world artifact identity");
        if ((retained_info.nFileIndexHigh == 0 && retained_info.nFileIndexLow == 0) ||
            ((retained_info.dwFileAttributes | published_info.dwFileAttributes) &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            retained_info.dwVolumeSerialNumber != published_info.dwVolumeSerialNumber ||
            retained_info.nFileIndexHigh != published_info.nFileIndexHigh ||
            retained_info.nFileIndexLow != published_info.nFileIndexLow)
            throw std::system_error(ERROR_INVALID_DATA,
                                    std::system_category(),
                                    "Published world artifact changed identity");
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (!::SetFileInformationByHandle(
                original.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not retire publication source link");
        // Disposition takes effect on close. Open-by-ID journal handles retain
        // identity without keeping this named source link alive.
        original.reset();
        source_retired = true;
        const auto deleting = ::ReOpenFile(
            linked.get(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_OPEN_REPARSE_POINT);
        if (deleting == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not retain created publication link for cleanup");
        linked.reset(deleting);
        linked_deletable = true;
        if (WindowsPublicationLinkCount(linked.get()) != 1)
            throw std::system_error(ERROR_INVALID_DATA,
                                    std::system_category(),
                                    "Could not verify retired publication source link count");
        if (WindowsPublicationPath(linked.get()) != bound_destination)
            throw std::system_error(ERROR_INVALID_DATA,
                                    std::system_category(),
                                    "Could not confirm world artifact publication name");
        const auto retirement_error = WindowsVacantPublicationNameError(original_name);
        if (retirement_error != ERROR_SUCCESS)
            throw std::system_error(static_cast<int>(retirement_error),
                                    std::system_category(),
                                    "Could not confirm world artifact source retirement");
        return linked.release();
    } catch (const std::exception& error) {
        // Roll back only the alias created by the successful link call. Keep a
        // bound destination handle across source close; reopening that handle
        // never looks up a replacement destination. If retirement completed,
        // first restore the source without replacing any new source occupant.
        try {
            if (linked.get() == INVALID_HANDLE_VALUE) {
                // Retry binding only for rollback while the original still
                // denies deletion of every alias. Never remove by pathname.
                linked.reset(::CreateFileW(bound_destination.c_str(),
                                           FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_FLAG_OPEN_REPARSE_POINT,
                                           nullptr));
                if (linked.get() == INVALID_HANDLE_VALUE)
                    throw std::runtime_error("Created publication link could not be bound");
            }
            original.reset();
            if (!linked_deletable) {
                const auto deleting = ::ReOpenFile(linked.get(),
                                                   DELETE | FILE_READ_ATTRIBUTES,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                   FILE_FLAG_OPEN_REPARSE_POINT);
                if (deleting == INVALID_HANDLE_VALUE)
                    throw std::system_error(
                        static_cast<int>(::GetLastError()),
                        std::system_category(),
                        "Could not reopen created publication link for rollback");
                linked.reset(deleting);
            }
            if (source_retired) {
                WindowsRestorePublicationSource(linked.get(), original_name);
                if (WindowsPublicationLinkCount(linked.get()) != 2)
                    throw std::runtime_error("Publication source recovery is unverified");
            }
            FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
            if (!::SetFileInformationByHandle(
                    linked.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                throw std::system_error(static_cast<int>(::GetLastError()),
                                        std::system_category(),
                                        "Could not retire created publication link");
        } catch (const std::exception& recovery_error) {
            throw WindowsPublicationRecoveryError(
                std::string(error.what()) + "; created destination retained for recovery: " +
                    Filesystem::GenericUtf8PathString(bound_destination) + "; " +
                    recovery_error.what(),
                verified_link);
        }
        throw;
    }
}

} // namespace Detail
#endif

// A journal for one exclusively created save. Writers reserve vacant names
// before publishing, including temporary files; absent names need no cleanup.
// Loading never populates this in-memory journal; process crashes retain files
// for inspection, not automatic ownership recovery. Call rollback after
// draining all readers/writers, or release after entry succeeds. Destruction never deletes.
class CreatedWorldArtifacts {
public:
    CreatedWorldArtifacts() = default;
    CreatedWorldArtifacts(const CreatedWorldArtifacts&) = delete;
    CreatedWorldArtifacts& operator=(const CreatedWorldArtifacts&) = delete;

    void create_root(const std::filesystem::path& path) {
        if (m_count != 0)
            throw std::runtime_error("A new world still has uncommitted artifacts.");
        auto root = std::filesystem::canonical(path.parent_path()) / path.filename();
        Artifact artifact{
            .path = std::move(root), .directory = true, .identity = {}, .retained = {}};
        if (std::filesystem::exists(std::filesystem::symlink_status(artifact.path)))
            throw std::runtime_error("Refusing to reuse an existing world save directory.");
        create_owned_directory(std::move(artifact));
    }

    void create_directory(const std::filesystem::path& path) {
        create_owned_directory(vacant_artifact(path, true));
    }

    void reserve_file(const std::filesystem::path& path) {
        auto artifact = vacant_artifact(path, false);
        m_artifacts[m_count++] = std::move(artifact);
    }

#if defined(_WIN32)
    using NativeFile = HANDLE;
#else
    using NativeFile = int;
#endif

    // Called immediately after exclusive creation, while the writer still owns
    // the native handle. Retain the object until release or completed removal
    // so an unlinked object's identity cannot be recycled. Windows writers
    // must allow read/write/delete sharing for publication and rollback.
    void file_created(const std::filesystem::path& path, NativeFile file) {
        auto& artifact = find_artifact(path);
        if (artifact.directory || artifact.identity)
            throw std::runtime_error("Invalid world file creation record.");
        artifact.retained = duplicate_file(file);
        artifact.identity = identity_from(artifact.retained.get());
    }

    // Retain the source before exclusive publication. Claim the destination
    // only after exclusive publication succeeds, before any caller's directory
    // flush. Even a racing hard link to the source remains unowned on refusal.
    void publish_file(const std::filesystem::path& temp, const std::filesystem::path& destination) {
        const auto& source = find_artifact(temp);
        auto& target = find_artifact(destination);
        validate_parents(source);
        validate_parents(target);
        if (!validate(source) || !source.identity || target.directory || target.identity ||
            std::filesystem::exists(std::filesystem::symlink_status(target.path)))
            throw std::runtime_error("Refusing to replace a world artifact destination.");
        target.retained = duplicate_file(source.retained.get());
#if defined(_WIN32)
        publish_windows(source, target);
#else
        publish_posix(source, target);
#endif
        target.identity = source.identity;
    }

    // Exclusively create a file and record its identity before the first byte.
    // Callers requiring durable publication use WorldSaveService.
    void write_file(const std::filesystem::path& path, const std::string& bytes) {
        reserve_file(path);
#if defined(_WIN32)
        ScopedFile native(::CreateFileW(path.c_str(),
                                        GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
        if (native.get() == INVALID_HANDLE_VALUE) {
            abandon_file_reservation();
            throw std::runtime_error("Could not exclusively create world artifact: " +
                                     Filesystem::GenericUtf8PathString(path));
        }
        file_created(path, native.get());
        const auto descriptor =
            ::_open_osfhandle(reinterpret_cast<intptr_t>(native.get()), _O_BINARY | _O_WRONLY);
        if (descriptor < 0)
            throw std::runtime_error("Could not open world artifact stream.");
        native.release(); // The CRT descriptor now owns the creating handle.
        auto* file = ::_fdopen(descriptor, "wb");
        if (!file) {
            ::_close(descriptor);
            throw std::runtime_error("Could not open world artifact stream.");
        }
#else
        auto* file = std::fopen(path.c_str(), "wbx");
#endif
        if (!file) {
            abandon_file_reservation();
            throw std::runtime_error("Could not exclusively create world artifact: " +
                                     Filesystem::GenericUtf8PathString(path));
        }
        bool written = false;
        try {
#if !defined(_WIN32)
            file_created(path, ::fileno(file));
#endif
            written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        } catch (...) {
            std::fclose(file);
            throw;
        }
        const bool closed = std::fclose(file) == 0;
        if (!written || !closed)
            throw std::runtime_error("Could not write world artifact: " +
                                     Filesystem::GenericUtf8PathString(path));
    }

    // Only for a failed exclusive file-create operation, which created nothing.
    void abandon_file_reservation() {
        if (m_count > 1 && !m_artifacts[m_count - 1].directory) {
            m_artifacts[--m_count] = Artifact{};
        }
    }

    void release() {
        for (std::size_t index = 0; index < m_count; ++index)
            m_artifacts[index] = Artifact{};
        m_count = 0;
    }

    // Preflight the entire journal and every owned directory before deleting
    // anything. Never follow symlinks, adopt unknown entries, or remove trees.
    // Keep the journal on failure so its diagnostics and retry remain possible.
    std::string rollback() {
        if (m_count == 0)
            return {};
        try {
            const auto& root = m_artifacts[0].path;
            if (std::filesystem::canonical(root.parent_path()) != root.parent_path())
                throw std::runtime_error("The save parent changed during world entry.");
            for (std::size_t index = 0; index < m_count; ++index) {
                const auto& artifact = m_artifacts[index];
                if (!validate(artifact))
                    continue;
                validate_parents(artifact);
                if (artifact.directory) {
                    for (const auto& child : std::filesystem::directory_iterator(artifact.path)) {
                        if (!tracked(child.path()))
                            throw std::runtime_error(
                                "Untracked world artifact: " +
                                Filesystem::GenericUtf8PathString(child.path()));
                    }
                }
            }
            for (std::size_t index = m_count; index != 0; --index) {
                auto& artifact = m_artifacts[index - 1];
                // Recheck the root and every owning directory as well as the
                // object immediately before removal, not only at preflight.
                validate_parents(artifact);
                if (validate(artifact))
                    remove_owned(artifact);
                // Windows completes deletion when the last handle closes,
                // before removing the owning directory. Forget the removed
                // identity too: a retry must refuse any later occupant.
                artifact.retained = ScopedFile{};
                artifact.identity.reset();
            }
            release();
            return {};
        } catch (const std::exception& error) {
            return "Could not roll back the new world: " + std::string(error.what());
        }
    }

private:
    struct Identity {
#if defined(_WIN32)
        DWORD volume = 0;
        DWORD file_high = 0;
        DWORD file_low = 0;
#else
        dev_t device = 0;
        ino_t inode = 0;
#endif
        bool operator==(const Identity&) const = default;
    };

    static Identity identity_from(NativeFile file) {
#if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(file, &info))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not identify world artifact");
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            throw std::runtime_error("Refusing a reparse point world artifact.");
        if (info.nFileIndexHigh == 0 && info.nFileIndexLow == 0)
            throw std::runtime_error("Unknown world artifact identity.");
        return {.volume = info.dwVolumeSerialNumber,
                .file_high = info.nFileIndexHigh,
                .file_low = info.nFileIndexLow};
#else
        struct stat info {};
        if (::fstat(file, &info) != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not identify world artifact");
        if (info.st_ino == 0)
            throw std::runtime_error("Unknown world artifact identity.");
        return {.device = info.st_dev, .inode = info.st_ino};
#endif
    }

    static Identity identity_at(const std::filesystem::path& path) {
#if defined(_WIN32)
        const auto file = ::CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not inspect world artifact");
        try {
            const auto identity = identity_from(file);
            ::CloseHandle(file);
            return identity;
        } catch (...) {
            ::CloseHandle(file);
            throw;
        }
#else
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not inspect world artifact");
        return {.device = info.st_dev, .inode = info.st_ino};
#endif
    }

    class ScopedFile {
    public:
        ScopedFile() = default;
        explicit ScopedFile(NativeFile native_file)
            : m_file(native_file) {}
        ~ScopedFile() {
#if defined(_WIN32)
            if (m_file != INVALID_HANDLE_VALUE)
                ::CloseHandle(m_file);
#else
            if (m_file >= 0)
                ::close(m_file);
#endif
        }
        ScopedFile(const ScopedFile&) = delete;
        ScopedFile& operator=(const ScopedFile&) = delete;
        ScopedFile(ScopedFile&& other) noexcept
            : m_file(other.release()) {}
        ScopedFile& operator=(ScopedFile&& other) noexcept {
            if (this != &other) {
                ScopedFile previous(other.release());
                std::swap(m_file, previous.m_file);
            }
            return *this;
        }
        NativeFile get() const {
            return m_file;
        }
        NativeFile release() {
            return std::exchange(m_file, kInvalidFile);
        }

    private:
#if defined(_WIN32)
        static inline const NativeFile kInvalidFile = INVALID_HANDLE_VALUE;
#else
        static constexpr NativeFile kInvalidFile = -1;
#endif
        NativeFile m_file = kInvalidFile;
    };

    struct Artifact {
        std::filesystem::path path;
        bool directory = false;
        std::optional<Identity> identity{};
        ScopedFile retained{};
    };

    static ScopedFile duplicate_file(NativeFile file) {
#if defined(_WIN32)
        // Open by ID to retain the object independently of its directory entry.
        // In particular, never keep the named directory-creation handle alive:
        // delete sharing alone does not permit moving its open descendants.
        FILE_ID_INFO identity{};
        if (!::GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not inspect world artifact for retention");
        if (std::all_of(std::begin(identity.FileId.Identifier),
                        std::end(identity.FileId.Identifier),
                        [](BYTE value) { return value == 0; }))
            throw std::runtime_error("Unknown world artifact retention identity.");
        // GetFinalPathNameByHandle with VOLUME_NAME_GUID supplies the volume
        // device name, independent of drive letters, mount points and 8.3 names.
        // Open that device explicitly: OpenFileById receives a volume handle.
        // The fixed buffer includes the Win32 path limit's terminating null.
        std::array<wchar_t, 32768> volume_name{};
        const auto length = ::GetFinalPathNameByHandleW(file,
                                                        volume_name.data(),
                                                        static_cast<DWORD>(volume_name.size()),
                                                        VOLUME_NAME_GUID | FILE_NAME_NORMALIZED);
        if (length == 0 || length >= volume_name.size())
            throw std::system_error(
                static_cast<int>(length == 0 ? ::GetLastError() : ERROR_FILENAME_EXCED_RANGE),
                std::system_category(),
                "Could not resolve world artifact volume");
        std::wstring volume_path(volume_name.data(), length);
        const auto separator = volume_path.find(L'\\', 4);
        if (!volume_path.starts_with(L"\\\\?\\Volume{") || separator == std::wstring::npos)
            throw std::runtime_error("Unknown world artifact volume name.");
        volume_path.resize(separator); // CreateFile opens a volume without a trailing slash.
        const ScopedFile volume(
            ::CreateFileW(volume_path.c_str(),
                          0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          0,
                          nullptr));
        if (volume.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not open world artifact volume");
        FILE_ID_DESCRIPTOR descriptor{};
        descriptor.dwSize = sizeof(descriptor);
        descriptor.Type = ExtendedFileIdType;
        descriptor.ExtendedFileId = identity.FileId;
        const auto retained = ::OpenFileById(volume.get(),
                                             &descriptor,
                                             FILE_READ_ATTRIBUTES,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             FILE_FLAG_BACKUP_SEMANTICS);
        if (retained == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not retain world artifact");
#else
        const auto retained = ::fcntl(file, F_DUPFD_CLOEXEC, 0);
        if (retained < 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not retain world artifact");
#endif
        return ScopedFile(retained);
    }

#if defined(_WIN32)
    static std::filesystem::path handle_path(NativeFile file) {
        return Detail::WindowsPublicationPath(file);
    }

    ScopedFile bind_publication_parent(const std::filesystem::path& path) {
        ScopedFile parent(::CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr));
        if (parent.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not bind publication directory");
        if (identity_from(parent.get()) != find_artifact(path).identity)
            throw std::runtime_error("World artifact publication directory changed identity.");
        return parent;
    }

    void publish_windows(const Artifact& source, Artifact& target) {
        const auto parent = bind_publication_parent(target.path.parent_path());
        // Keep both named parents fixed until publication and name inspection
        // complete, including when the reserved source is in another directory.
        const auto source_parent = bind_publication_parent(source.path.parent_path());
        // Use the handle's normalized, extended Win32 name. The caller may
        // supply forward slashes or an 8.3 ancestor. Resolve the bound names and
        // preserve UTF-16 throughout.
        auto destination = handle_path(parent.get()) / target.path.filename();
        destination.make_preferred();
        ScopedFile bound(::CreateFileW(source.path.c_str(),
                                       FILE_READ_ATTRIBUTES | DELETE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr));
        if (bound.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not bind world artifact for publication");
        if (identity_from(bound.get()) != source.identity)
            throw std::runtime_error("World artifact changed identity: " +
                                     Filesystem::GenericUtf8PathString(source.path));
        try {
            const ScopedFile published(Detail::WindowsPublishWithoutReplacement(
                bound.release(), source.path, destination, FILE_READ_ATTRIBUTES | DELETE));
        } catch (const Detail::WindowsPublicationRecoveryError& error) {
            // A verified alias created by this call is safe to retain for
            // rollback even when source retirement or recovery failed. This
            // records cleanup ownership, never a successful publication.
            if (error.verified_link())
                target.identity = source.identity;
            throw;
        }
    }
#endif

#if !defined(_WIN32)
    void publish_posix(const Artifact& source, const Artifact& target) {
        const ScopedFile source_parent(::open(source.path.parent_path().c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        const ScopedFile target_parent(::open(target.path.parent_path().c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (source_parent.get() < 0 || target_parent.get() < 0)
            throw std::system_error(errno, std::generic_category(), "Could not bind save parent");
        if (identity_from(source_parent.get()) !=
                find_artifact(source.path.parent_path()).identity ||
            identity_from(target_parent.get()) != find_artifact(target.path.parent_path()).identity)
            throw std::runtime_error("World artifact publication directory changed identity.");
        // Bind the entry in a private, single-entry directory before checking
        // its ID. Publication then uses that same entry and bound directories,
        // rather than looking up the public source pathname a second time.
        const Quarantine quarantine(source_parent.get());
        const ScopedFile container(::openat(source_parent.get(),
                                            quarantine.name().c_str(),
                                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (container.get() < 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not bind publication stage");
        const auto source_name = source.path.filename();
        const auto target_name = target.path.filename();
        if (Detail::RenameWithoutReplacement(
                source_parent.get(), source_name.c_str(), container.get(), "artifact") != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not bind world artifact for publication");
        try {
            struct stat info {};
            if (::fstatat(container.get(), "artifact", &info, AT_SYMLINK_NOFOLLOW) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "Could not identify publication stage");
            const Identity moved{.device = info.st_dev, .inode = info.st_ino};
            if (!S_ISREG(info.st_mode) || moved != source.identity)
                throw std::runtime_error("World artifact changed identity: " +
                                         Filesystem::GenericUtf8PathString(source.path));
            if (Detail::RenameWithoutReplacement(
                    container.get(), "artifact", target_parent.get(), target_name.c_str()) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "Could not exclusively publish world artifact");
        } catch (const std::exception& error) {
            if (restore_quarantined(container.get(), source_parent.get(), source_name.c_str()) != 0)
                throw std::runtime_error(std::string(error.what()) + "; retained in quarantine: " +
                                         Filesystem::GenericUtf8PathString(
                                             source.path.parent_path() / quarantine.name()));
            throw;
        }
    }
#endif

    void create_owned_directory(Artifact artifact) {
#if defined(_WIN32)
        // Win32 CreateDirectory followed by CreateFile cannot identify the
        // directory we created. NtCreateFile exclusively creates AND returns
        // the directory handle, relative to a bound parent directory.
        const ScopedFile parent(
            ::CreateFileW(artifact.path.parent_path().c_str(),
                          FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr));
        if (parent.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not open save parent");
        auto name = artifact.path.filename().native();
        // UNICODE_STRING lengths are bytes in a USHORT, without a terminator.
        if (name.size() > 32767)
            throw std::runtime_error("World directory name is too long.");
        const auto bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        UNICODE_STRING native_name{.Length = bytes, .MaximumLength = bytes, .Buffer = name.data()};
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = static_cast<ULONG>(sizeof(attributes));
        attributes.RootDirectory = parent.get();
        attributes.ObjectName = &native_name;
        attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE.
        using CreateDirectoryFunction = NTSTATUS(NTAPI*)(PHANDLE,
                                                         ACCESS_MASK,
                                                         POBJECT_ATTRIBUTES,
                                                         PIO_STATUS_BLOCK,
                                                         PLARGE_INTEGER,
                                                         ULONG,
                                                         ULONG,
                                                         ULONG,
                                                         ULONG,
                                                         PVOID,
                                                         ULONG);
        const auto library = ::GetModuleHandleW(L"ntdll.dll");
        const FARPROC entry = library ? ::GetProcAddress(library, "NtCreateFile") : nullptr;
        const auto create = std::bit_cast<CreateDirectoryFunction>(entry);
        if (!create)
            throw std::runtime_error("Exclusive directory creation is unavailable.");
        HANDLE handle = INVALID_HANDLE_VALUE;
        IO_STATUS_BLOCK status{};
        // Native create disposition FILE_CREATE (2) refuses an existing name.
        // FILE_DIRECTORY_FILE (1) requires a directory rather than a file;
        // FILE_SYNCHRONOUS_IO_NONALERT (0x20) completes creation before return.
        const auto result = create(&handle,
                                   FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                   &attributes,
                                   &status,
                                   nullptr,
                                   FILE_ATTRIBUTE_NORMAL,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   2,
                                   1 | 0x20,
                                   nullptr,
                                   0);
        if (result < 0)
            throw std::runtime_error("Could not exclusively create world directory (status " +
                                     std::to_string(result) + ").");
        m_artifacts[m_count++] = std::move(artifact);
        auto& recorded = m_artifacts[m_count - 1];
        recorded.retained = ScopedFile(handle);
        recorded.retained = duplicate_file(recorded.retained.get());
        recorded.identity = identity_from(recorded.retained.get());
#else
        // POSIX mkdir has no creating descriptor. Prepare an unpredictable,
        // private (0700) staging directory, bind its descriptor and identity,
        // then publish with an exclusive rename. Never infer ownership from
        // the public name after mkdir: it may already name a replacement.
        auto staging = (artifact.path.parent_path() / ".world-create-XXXXXX").native();
        if (!::mkdtemp(staging.data()))
            throw std::system_error(
                errno, std::generic_category(), "Could not stage world directory");
        auto destination = std::move(artifact.path);
        artifact.path = staging;
        m_artifacts[m_count++] = std::move(artifact);
        auto& recorded = m_artifacts[m_count - 1];
        recorded.retained = ScopedFile(
            ::open(recorded.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (recorded.retained.get() < 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not bind staged world directory");
        recorded.identity = identity_from(recorded.retained.get());
        validate(recorded);
        const auto result = Detail::RenameWithoutReplacement(
            AT_FDCWD, recorded.path.c_str(), AT_FDCWD, destination.c_str());
        if (result != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not publish world directory");
        recorded.path = std::move(destination);
#endif
        validate_parents(recorded);
        if (!validate(recorded))
            throw std::runtime_error(
                "Created world directory disappeared before publication completed.");
    }

#if !defined(_WIN32)
    // Restoring a quarantined entry must not overwrite a second replacement.
    static int restore_quarantined(int from, int to, const char* name) {
        return Detail::RenameWithoutReplacement(from, "artifact", to, name);
    }
#endif

#if !defined(_WIN32)
    class Quarantine {
    public:
        explicit Quarantine(int parent)
            : m_parent(parent) {
            static std::atomic<std::uint64_t> sequence{0};
            // Bound collision retries; each private container holds just one entry.
            for (unsigned attempt = 0; attempt < 128; ++attempt) {
                auto candidate = ".world-rollback-" + std::to_string(::getpid()) + "-" +
                                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
                if (::mkdirat(m_parent, candidate.c_str(), S_IRWXU) == 0) {
                    m_name = std::move(candidate);
                    return;
                }
                if (errno != EEXIST)
                    throw std::system_error(
                        errno, std::generic_category(), "Could not quarantine world artifact");
            }
            throw std::runtime_error("Could not reserve world artifact quarantine.");
        }
        ~Quarantine() {
            // Only an empty container is removed. A failed restore retains
            // its entry for recovery, never recursively deletes it.
            ::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR);
        }
        Quarantine(const Quarantine&) = delete;
        Quarantine& operator=(const Quarantine&) = delete;
        const std::string& name() const {
            return m_name;
        }

    private:
        int m_parent;
        std::string m_name{};
    };
#endif

    void remove_owned(const Artifact& artifact) const {
        if (!artifact.identity)
            throw std::runtime_error("Unowned world artifact: " +
                                     Filesystem::GenericUtf8PathString(artifact.path));
#if defined(_WIN32)
        // Do not request DELETE_ON_CLOSE before checking the opened object's ID.
        const ScopedFile owned(
            ::CreateFileW(artifact.path.c_str(),
                          FILE_READ_ATTRIBUTES | DELETE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                          nullptr));
        if (owned.get() == INVALID_HANDLE_VALUE) {
            const auto error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return;
            throw std::system_error(static_cast<int>(error),
                                    std::system_category(),
                                    "Could not open world artifact for removal");
        }
        if (identity_from(owned.get()) != *artifact.identity)
            throw std::runtime_error("World artifact changed identity: " +
                                     Filesystem::GenericUtf8PathString(artifact.path));
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (!::SetFileInformationByHandle(
                owned.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not remove world artifact");
#else
        const ScopedFile parent(::open(artifact.path.parent_path().c_str(),
                                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0)
            throw std::system_error(errno, std::generic_category(), "Could not open save parent");
        for (std::size_t index = 0; index < m_count; ++index) {
            const auto& candidate = m_artifacts[index];
            if (candidate.directory && candidate.path == artifact.path.parent_path() &&
                (!candidate.identity || identity_from(parent.get()) != *candidate.identity))
                throw std::runtime_error("World artifact parent changed identity: " +
                                         Filesystem::GenericUtf8PathString(candidate.path));
        }

        const Quarantine quarantine(parent.get());
        const ScopedFile container(::openat(parent.get(),
                                            quarantine.name().c_str(),
                                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (container.get() < 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not open world artifact quarantine");
        const auto name = artifact.path.filename();
        if (::renameat(parent.get(), name.c_str(), container.get(), "artifact") != 0) {
            if (errno == ENOENT)
                return;
            throw std::system_error(
                errno, std::generic_category(), "Could not quarantine world artifact");
        }
        try {
            struct stat info {};
            if (::fstatat(container.get(), "artifact", &info, AT_SYMLINK_NOFOLLOW) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "Could not inspect quarantined artifact");
            const Identity moved{.device = info.st_dev, .inode = info.st_ino};
            if (moved != *artifact.identity)
                throw std::runtime_error("World artifact changed identity: " +
                                         Filesystem::GenericUtf8PathString(artifact.path));
            if (::unlinkat(container.get(), "artifact", artifact.directory ? AT_REMOVEDIR : 0) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "Could not remove quarantined artifact");
        } catch (const std::exception& error) {
            if (restore_quarantined(container.get(), parent.get(), name.c_str()) != 0)
                throw std::runtime_error(std::string(error.what()) + "; retained in quarantine: " +
                                         Filesystem::GenericUtf8PathString(
                                             artifact.path.parent_path() / quarantine.name()));
            throw;
        }
#endif
    }

    Artifact& find_artifact(const std::filesystem::path& path) {
        const auto normalized = std::filesystem::canonical(path.parent_path()) / path.filename();
        for (std::size_t index = 0; index < m_count; ++index)
            if (m_artifacts[index].path == normalized)
                return m_artifacts[index];
        throw std::runtime_error("Unreserved world artifact: " +
                                 Filesystem::GenericUtf8PathString(path));
    }

    static bool validate(const Artifact& artifact) {
        const auto status = std::filesystem::symlink_status(artifact.path);
        if (!std::filesystem::exists(status))
            return false;
        if (artifact.directory ? !std::filesystem::is_directory(status)
                               : !std::filesystem::is_regular_file(status))
            throw std::runtime_error("World artifact changed type: " +
                                     Filesystem::GenericUtf8PathString(artifact.path));
        if (!artifact.identity || identity_at(artifact.path) != *artifact.identity)
            throw std::runtime_error("World artifact changed identity: " +
                                     Filesystem::GenericUtf8PathString(artifact.path));
        return true;
    }

    void validate_parents(const Artifact& artifact) const {
        const auto& root = m_artifacts[0].path;
        if (std::filesystem::canonical(root.parent_path()) != root.parent_path())
            throw std::runtime_error("The save parent changed during world entry.");
        auto path = artifact.path.parent_path();
        while (path != root.parent_path()) {
            bool owned = false;
            for (std::size_t index = 0; index < m_count; ++index) {
                const auto& parent = m_artifacts[index];
                if (parent.directory && parent.path == path) {
                    validate(parent);
                    owned = true;
                    break;
                }
            }
            if (!owned)
                throw std::runtime_error("Unowned world artifact parent: " +
                                         Filesystem::GenericUtf8PathString(path));
            path = path.parent_path();
        }
    }

    Artifact vacant_artifact(const std::filesystem::path& path, bool directory) const {
        if (m_count == 0 || m_count == m_artifacts.size())
            throw std::runtime_error("Invalid world artifact journal capacity.");
        const auto normalized = std::filesystem::canonical(path.parent_path()) / path.filename();
        bool owned_parent = false;
        for (std::size_t index = 0; index < m_count; ++index) {
            const auto& artifact = m_artifacts[index];
            if (artifact.directory && artifact.path == normalized.parent_path() &&
                validate(artifact))
                owned_parent = true;
        }
        if (!owned_parent || tracked(normalized) ||
            std::filesystem::exists(std::filesystem::symlink_status(normalized)))
            throw std::runtime_error("Refusing to claim a world artifact: " +
                                     Filesystem::GenericUtf8PathString(normalized));
        return {.path = normalized, .directory = directory, .identity = {}, .retained = {}};
    }

    bool tracked(const std::filesystem::path& path) const {
        for (std::size_t index = 0; index < m_count; ++index)
            if (m_artifacts[index].path == path)
                return true;
        return false;
    }

    // Bound initial creation by its full artifact set. Renamed temporary files
    // retain their journal entries until release or rollback. At most one
    // retained native handle per entry, including both publication names.
    static constexpr std::size_t kCreationDirectories = 3;   // Save root, chunks, region.
    static constexpr std::size_t kCreationDurableFiles = 3;  // Preset, metadata, ledger.
    static constexpr std::size_t kEntriesPerDurableFile = 2; // Destination and temporary.
    static constexpr std::size_t kCreationArtifactCapacity =
        kCreationDirectories + kCreationDurableFiles * kEntriesPerDurableFile;
    static_assert(kCreationArtifactCapacity == 9);
    std::array<Artifact, kCreationArtifactCapacity> m_artifacts{};
    std::size_t m_count = 0;
};

} // namespace Luminumbra::Persistence
