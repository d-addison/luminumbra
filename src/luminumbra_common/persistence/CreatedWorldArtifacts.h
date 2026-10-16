#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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
        Artifact artifact{.path = std::move(root), .directory = true, .identity = {}};
        if (!std::filesystem::create_directory(artifact.path))
            throw std::runtime_error("Refusing to reuse an existing world save directory.");
        m_artifacts[m_count++] = std::move(artifact);
        m_artifacts[m_count - 1].identity = identity_at(m_artifacts[m_count - 1].path);
    }

    void create_directory(const std::filesystem::path& path) {
        auto artifact = vacant_artifact(path, true);
        if (!std::filesystem::create_directory(artifact.path))
            throw std::runtime_error("Refusing to reuse a world artifact directory.");
        m_artifacts[m_count++] = std::move(artifact);
        m_artifacts[m_count - 1].identity = identity_at(m_artifacts[m_count - 1].path);
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
    // the native handle. Never infer ownership by reopening the file's name.
    void file_created(const std::filesystem::path& path, NativeFile file) {
        auto& artifact = find_artifact(path);
        if (artifact.directory || artifact.identity)
            throw std::runtime_error("Invalid world file creation record.");
        artifact.identity = identity_from(file);
    }

    // Reserve the destination's identity before rename: a successful rename
    // keeps the temporary object's identity, even if the later directory flush fails.
    void publishing_file(const std::filesystem::path& temp,
                         const std::filesystem::path& destination) {
        const auto& source = find_artifact(temp);
        auto& target = find_artifact(destination);
        validate_parents(source);
        validate_parents(target);
        if (!validate(source) || !source.identity || target.directory || target.identity ||
            std::filesystem::exists(std::filesystem::symlink_status(target.path)))
            throw std::runtime_error("Refusing to replace a world artifact destination.");
        target.identity = source.identity;
    }

    // Exclusively create a file and record its identity before the first byte.
    // Callers requiring durable publication use WorldSaveService.
    void write_file(const std::filesystem::path& path, const std::string& bytes) {
        reserve_file(path);
#if defined(_WIN32)
        std::FILE* file = nullptr;
        ::_wfopen_s(&file, path.c_str(), L"wbx");
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
#if defined(_WIN32)
            file_created(path, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(file))));
#else
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
                const auto& artifact = m_artifacts[index - 1];
                // Recheck the root and every owning directory as well as the
                // object immediately before removal, not only at preflight.
                validate_parents(artifact);
                if (!validate(artifact))
                    continue;
                remove_owned(artifact);
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
        return {.volume = info.dwVolumeSerialNumber,
                .file_high = info.nFileIndexHigh,
                .file_low = info.nFileIndexLow};
#else
        struct stat info {};
        if (::fstat(file, &info) != 0)
            throw std::system_error(
                errno, std::generic_category(), "Could not identify world artifact");
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

    struct Artifact {
        std::filesystem::path path;
        bool directory = false;
        std::optional<Identity> identity{};
    };

    class ScopedFile {
    public:
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
        NativeFile get() const {
            return m_file;
        }

    private:
        NativeFile m_file;
    };

#if !defined(_WIN32)
    // Restoring a quarantined entry must not overwrite a second replacement.
#if defined(__linux__) || defined(__APPLE__)
    static int restore_quarantined(int from, int to, const char* name) {
#if defined(__linux__)
        return ::renameat2(from, "artifact", to, name, RENAME_NOREPLACE);
#else
        return ::renameatx_np(from, "artifact", to, name, RENAME_EXCL);
#endif
    }
#else
    static int restore_quarantined(int, int, const char*) {
        // Keep the entry quarantined on platforms without exclusive rename.
        errno = ENOTSUP;
        return -1;
    }
#endif
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
        return {.path = normalized, .directory = directory, .identity = {}};
    }

    bool tracked(const std::filesystem::path& path) const {
        for (std::size_t index = 0; index < m_count; ++index)
            if (m_artifacts[index].path == path)
                return true;
        return false;
    }

    // Initial creation owns at most eight artifacts: save root, preset,
    // metadata + temp, chunks + region directories, and ledger + temp.
    std::array<Artifact, 8> m_artifacts{};
    std::size_t m_count = 0;
};

} // namespace Luminumbra::Persistence
