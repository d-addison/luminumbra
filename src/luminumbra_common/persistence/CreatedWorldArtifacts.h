#pragma once

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

// A journal for one exclusively created save. Writers reserve vacant names
// before publishing, including temporary files; absent names need no cleanup.
// Loading never populates this journal. Call rollback only after draining all
// readers/writers, or release after entry succeeds. Destruction never deletes.
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
    // only after rename succeeds, before any caller's directory flush. Even a
    // racing hard link to the source must remain unowned when rename refuses.
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
        if (Detail::RenameWithoutReplacement(
                AT_FDCWD, temp.c_str(), AT_FDCWD, destination.c_str()) != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not exclusively publish world artifact");
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
                                     path.string());
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
                                     path.string());
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
            throw std::runtime_error("Could not write world artifact: " + path.string());
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
                            throw std::runtime_error("Untracked world artifact: " +
                                                     child.path().string());
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
        // A duplicate shares the writer's file object and its data access,
        // which prevents renaming an ancestor on Windows even with share-delete.
        // Reopen the SAME object for metadata only; never reopen its pathname.
        const auto retained = ::ReOpenFile(
            file, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
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
    static void publish_windows(const Artifact& source, const Artifact& target) {
        const auto& name = target.path.native();
        // Windows paths contain at most 32767 UTF-16 code units. Check before
        // allocating the variable-length native rename record (less than 65 KiB).
        if (name.size() > 32767)
            throw std::runtime_error("World artifact destination is too long.");
        const auto name_bytes = name.size() * sizeof(wchar_t);
        const auto record_bytes =
            (std::max)(sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName) + name_bytes);
        std::vector<std::byte> storage(record_bytes);
        auto* rename_info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename_info->ReplaceIfExists = FALSE;
        rename_info->RootDirectory = nullptr;
        rename_info->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(rename_info->FileName, name.data(), name_bytes);
        const ScopedFile bound(::CreateFileW(source.path.c_str(),
                                             FILE_READ_ATTRIBUTES | DELETE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_OPEN_REPARSE_POINT,
                                             nullptr));
        if (bound.get() == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not bind world artifact for publication");
        if (identity_from(bound.get()) != source.identity)
            throw std::runtime_error("World artifact changed identity: " + source.path.string());
        // MoveFileEx can report success for two names of the same file without
        // moving either. Native no-replace rename must refuse that collision too.
        if (!::SetFileInformationByHandle(
                bound.get(), FileRenameInfo, rename_info, static_cast<DWORD>(record_bytes)))
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(),
                                    "Could not exclusively publish world artifact");
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
            throw std::runtime_error("Unowned world artifact: " + artifact.path.string());
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
            throw std::runtime_error("World artifact changed identity: " + artifact.path.string());
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
                                         candidate.path.string());
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
                                         artifact.path.string());
            if (::unlinkat(container.get(), "artifact", artifact.directory ? AT_REMOVEDIR : 0) != 0)
                throw std::system_error(
                    errno, std::generic_category(), "Could not remove quarantined artifact");
        } catch (const std::exception& error) {
            if (restore_quarantined(container.get(), parent.get(), name.c_str()) != 0)
                throw std::runtime_error(
                    std::string(error.what()) + "; retained in quarantine: " +
                    (artifact.path.parent_path() / quarantine.name()).string());
            throw;
        }
#endif
    }

    Artifact& find_artifact(const std::filesystem::path& path) {
        const auto normalized = std::filesystem::canonical(path.parent_path()) / path.filename();
        for (std::size_t index = 0; index < m_count; ++index)
            if (m_artifacts[index].path == normalized)
                return m_artifacts[index];
        throw std::runtime_error("Unreserved world artifact: " + path.string());
    }

    static bool validate(const Artifact& artifact) {
        const auto status = std::filesystem::symlink_status(artifact.path);
        if (!std::filesystem::exists(status))
            return false;
        if (artifact.directory ? !std::filesystem::is_directory(status)
                               : !std::filesystem::is_regular_file(status))
            throw std::runtime_error("World artifact changed type: " + artifact.path.string());
        if (!artifact.identity || identity_at(artifact.path) != *artifact.identity)
            throw std::runtime_error("World artifact changed identity: " + artifact.path.string());
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
                throw std::runtime_error("Unowned world artifact parent: " + path.string());
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
            throw std::runtime_error("Refusing to claim a world artifact: " + normalized.string());
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
