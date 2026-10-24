#pragma once

#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/GraphicsPresets.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Luminumbra::Client::Measurement {

// Stream bytes without allocating in proportion to asset size. Binary reads
// preserve CRLF and every other byte identically on Windows and POSIX.
inline std::uintmax_t append_content_digest(std::istream& input, std::uint64_t& hash) {
    std::array<char, 65536> buffer{}; // At most 64 KiB of asset bytes in memory.
    std::uintmax_t bytes = 0;
    do {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::uintmax_t>(input.gcount());
        if (count > std::numeric_limits<std::uintmax_t>::max() - bytes)
            throw std::runtime_error("workload content size overflow");
        bytes += count;
        for (std::streamsize index = 0; index < input.gcount(); ++index)
            hash = (hash ^ static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)])) *
                   1099511628211ull;
    } while (input);
    if (!input.eof() || input.bad())
        throw std::runtime_error("cannot read workload content");
    return bytes;
}

inline std::string content_digest_hex(std::uint64_t hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4)
        result += digits[(hash >> shift) & 15u];
    return result;
}

inline std::string content_digest(std::istream& input) {
    std::uint64_t hash = 14695981039346656037ull;
    append_content_digest(input, hash);
    return content_digest_hex(hash);
}

inline std::string content_digest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open workload content");
    return content_digest(input);
}

// Open once, reject special files through that handle, and bound reads even if a
// writer grows the file. Nonblocking POSIX opens cannot hang on a replaced FIFO.
class SavedFileInput {
public:
    explicit SavedFileInput(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle_ == INVALID_HANDLE_VALUE || GetFileType(handle_) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            close();
            throw std::runtime_error("unsupported workload save file");
        }
        size_ = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
        handle_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
        struct stat info {};
        if (handle_ < 0 || ::fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) {
            close();
            throw std::runtime_error("unsupported workload save file");
        }
        size_ = static_cast<std::uint64_t>(info.st_size);
#endif
    }
    SavedFileInput(const SavedFileInput&) = delete;
    SavedFileInput& operator=(const SavedFileInput&) = delete;
    ~SavedFileInput() {
        close();
    }
    std::uint64_t size() const {
        return size_;
    }
    std::size_t read(std::array<char, 65536>& buffer) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            throw std::runtime_error("cannot read workload save");
        return count;
#else
        ssize_t count = 0;
        do {
            count = ::read(handle_, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw std::runtime_error("cannot read workload save");
        return static_cast<std::size_t>(count);
#endif
    }

private:
    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (handle_ >= 0)
            ::close(handle_);
        handle_ = -1;
#endif
    }
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
    std::uint64_t size_ = 0;
};

struct SavedFileBytes {
    std::string relative;
    std::string bytes;
};

// Bind sorted UTF-8 paths and the exact bytes read from each owning handle.
// A snapshot retains those same bytes; paths in the source are never reopened
// by its consumer. At most 65536 entries, 4096 bytes/path, 64 levels, 512 MiB total.
inline std::string read_saved_world(const std::filesystem::path& directory,
                                    std::vector<SavedFileBytes>* snapshot = nullptr) {
    if (std::filesystem::is_symlink(directory) || !std::filesystem::is_directory(directory))
        throw std::runtime_error("unsupported workload save directory");
    const auto canonical_directory = std::filesystem::canonical(directory);
    std::vector<std::string> paths;
    std::size_t entries = 0;
    for (auto iterator = std::filesystem::recursive_directory_iterator(directory);
         iterator != std::filesystem::recursive_directory_iterator();
         ++iterator) {
        if (entries == 65536 || iterator.depth() > 64)
            throw std::runtime_error("workload save inventory exceeds bounds");
        ++entries;
        const auto& entry = *iterator;
        const auto relative = entry.path().lexically_relative(directory).generic_u8string();
        if (relative.size() > 4096 || relative.find_first_of(u8"\\:") != std::u8string::npos ||
            entry.is_symlink() ||
            std::filesystem::canonical(entry.path()) !=
                canonical_directory / std::filesystem::path(relative))
            throw std::runtime_error("unsupported workload save path");
        if (entry.is_regular_file())
            paths.emplace_back(reinterpret_cast<const char*>(relative.data()), relative.size());
        else if (!entry.is_directory())
            throw std::runtime_error("unsupported workload save file");
    }
    std::sort(paths.begin(), paths.end());
    std::uint64_t hash = 14695981039346656037ull;
    constexpr std::uint64_t max_bytes = 512ull * 1024 * 1024;
    std::uint64_t total = 0;
    for (const auto& relative : paths) {
        SavedFileInput input(directory / Filesystem::PathFromUtf8(relative));
        const auto size = input.size();
        if (size > max_bytes - total)
            throw std::runtime_error("workload save bytes exceed bounds");
        total += size;
        std::istringstream header(std::to_string(relative.size()) + ":" + relative +
                                  std::to_string(size) + ":");
        append_content_digest(header, hash);
        std::string bytes;
        if (snapshot)
            bytes.reserve(static_cast<std::size_t>(size));
        std::array<char, 65536> buffer{};
        std::uint64_t consumed = 0;
        for (;;) {
            const auto count = input.read(buffer);
            if (count == 0)
                break;
            if (count > size - consumed)
                throw std::runtime_error("workload save changed while reading");
            consumed += count;
            for (std::size_t index = 0; index < count; ++index)
                hash = (hash ^ static_cast<unsigned char>(buffer[index])) * 1099511628211ull;
            if (snapshot)
                bytes.append(buffer.data(), count);
        }
        if (consumed != size)
            throw std::runtime_error("workload save changed while reading");
        if (snapshot)
            snapshot->push_back({relative, std::move(bytes)});
    }
    return content_digest_hex(hash);
}

inline std::string saved_world_digest(const std::filesystem::path& directory) {
    return read_saved_world(directory);
}
inline void require_saved_world_digest(const std::filesystem::path& directory,
                                       std::string_view expected) {
    if (expected.empty() || saved_world_digest(directory) != expected)
        throw std::runtime_error("workload saved authority changed before reload");
}

// Private materialization of an immutable byte snapshot for synchronous loaders.
// Verify all owned bytes before creating files. The live save is never modified;
// concurrent writers there cannot affect any subsequent loader read here.
class VerifiedSaveSnapshot {
public:
    VerifiedSaveSnapshot(const std::filesystem::path& directory,
                         std::string_view expected,
                         const std::string& world_id) {
        if (world_id.empty() || world_id == "." || world_id == ".." ||
            world_id.find_first_of("/\\:") != std::string::npos || expected.empty())
            throw std::runtime_error("invalid workload snapshot identity");
        std::vector<SavedFileBytes> files;
        if (read_saved_world(directory, &files) != expected || files.empty())
            throw std::runtime_error("workload saved authority changed before reload");
        std::random_device random;
        const auto parent = std::filesystem::temp_directory_path();
        // Refuse after 16 name collisions; never reuse a pre-existing directory.
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto candidate = parent / ("render-save-" + std::to_string(random()) + "-" +
                                             std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                root_ = candidate;
                break;
            }
        }
        if (root_.empty())
            throw std::runtime_error("cannot create workload snapshot");
        try {
            identity_ = directory_identity(root_);
            if (identity_ == std::array<std::uint64_t, 2>{})
                throw std::runtime_error("cannot identify workload snapshot");
            std::filesystem::permissions(
                root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
            const auto save = root_ / "worlds/saves" / world_id;
            for (const auto& file : files) {
                const auto path = save / Filesystem::PathFromUtf8(file.relative);
                std::filesystem::create_directories(path.parent_path());
                std::ofstream output(path, std::ios::binary);
                output.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
                output.close();
                if (!output)
                    throw std::runtime_error("cannot write workload snapshot");
            }
            // Publish a read-only private materialization to the synchronous loader. At most 65539
            // entries including the three loader-root directories are visited.
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root_))
                std::filesystem::permissions(entry.path(),
                                             entry.is_directory()
                                                 ? std::filesystem::perms::owner_read |
                                                       std::filesystem::perms::owner_exec
                                                 : std::filesystem::perms::owner_read,
                                             std::filesystem::perm_options::replace);
            std::filesystem::permissions(root_,
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec,
                                         std::filesystem::perm_options::replace);
        } catch (...) {
            cleanup();
            throw;
        }
    }
    VerifiedSaveSnapshot(const VerifiedSaveSnapshot&) = delete;
    VerifiedSaveSnapshot& operator=(const VerifiedSaveSnapshot&) = delete;
    ~VerifiedSaveSnapshot() {
        cleanup();
    }
    const std::filesystem::path& root() const {
        return root_;
    }

private:
    static std::array<std::uint64_t, 2> directory_identity(const std::filesystem::path& path) {
#ifdef _WIN32
        const auto handle = CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return {};
        BY_HANDLE_FILE_INFORMATION info{};
        const bool valid = GetFileInformationByHandle(handle, &info) &&
                           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        CloseHandle(handle);
        if (!valid)
            return {};
        return {info.dwVolumeSerialNumber,
                (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
        struct stat info {};
        if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            return {};
        return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
    }
    void cleanup() noexcept {
        // Remove only the directory this call created, checking its identity at removal.
        if (identity_ != std::array<std::uint64_t, 2>{} && directory_identity(root_) == identity_) {
            std::error_code error;
            std::filesystem::permissions(root_,
                                         std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace,
                                         error);
            auto iterator = std::filesystem::recursive_directory_iterator(root_, error);
            const auto end = std::filesystem::recursive_directory_iterator();
            while (!error && iterator != end) {
                std::filesystem::permissions(iterator->path(),
                                             std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace,
                                             error);
                if (!error)
                    iterator.increment(error);
            }
            if (!error && directory_identity(root_) == identity_)
                std::filesystem::remove_all(root_, error);
        }
    }
    std::filesystem::path root_;
    std::array<std::uint64_t, 2> identity_{};
};

// Conservatively include data, resources, config, installed assets and audio:
// scatter archetypes can reference models/textures (including asset packs),
// presets can reference tuning, and UI documents can reference styles/fonts.
// Sorting by portable relative path
// makes enumeration order irrelevant. Only used outside measured frame intervals.
inline nlohmann::json workload_content(const std::filesystem::path& root) {
    Filesystem::CaptureContentPaths containment;
    containment.enable(root);
    nlohmann::json result = nlohmann::json::object();
    // Bound manifest storage to 65536 entries with 4096-byte relative paths
    // (including directory markers); directories count toward the same bound.
    // Bound directory depth to 64, independently of filesystem traversal order.
    std::size_t entries = 0;
    for (const char* tree : Filesystem::kCaptureContentTrees) {
        // Public packages omit audio. Record absence so an empty or populated
        // audio tree appearing later also changes the render identity.
        if (tree == Filesystem::kOptionalCaptureContentTree &&
            !std::filesystem::exists(root / tree) && !std::filesystem::is_symlink(root / tree)) {
            if (entries == 65536)
                throw std::runtime_error("workload content manifest exceeds bounds");
            ++entries;
            result[tree] = nullptr;
            continue;
        }
        if (std::filesystem::is_symlink(root / tree) || !std::filesystem::is_directory(root / tree))
            throw std::runtime_error("missing workload content tree");
        for (auto iterator = std::filesystem::recursive_directory_iterator(root / tree);
             iterator != std::filesystem::recursive_directory_iterator();
             ++iterator) {
            if (entries == 65536 || iterator.depth() > 64)
                throw std::runtime_error("workload content manifest exceeds bounds");
            ++entries;
            const auto& entry = *iterator;
            const auto relative = entry.path().lexically_relative(root).generic_u8string();
            if (relative.size() > 4096 || entry.is_symlink() || !containment.allows(entry.path()))
                throw std::runtime_error("unsupported workload content path");
            const std::string path(reinterpret_cast<const char*>(relative.data()), relative.size());
            if (path.find_first_of("\\:") != std::string::npos)
                throw std::runtime_error("unsupported workload content path");
            if (entry.is_regular_file())
                result[path] = content_digest(entry.path());
            else if (entry.is_directory()) {
                if (path.size() == 4096)
                    throw std::runtime_error("unsupported workload content path");
                // A file key cannot end in '/', so empty directories are unambiguous.
                result[path + '/'] = nullptr;
            } else
                throw std::runtime_error("unsupported workload content file");
        }
    }
    return result;
}

// Enumerate the same generated registry that defines the runtime queries.
// Parameters retain their resolved configured/default values even when their
// owning flag is disabled, so captures conservatively retain all config inputs.
inline nlohmann::json render_systems(const luminumbra::core::SystemConfig& config) {
    using luminumbra::core::SysKey;
    using luminumbra::core::SysParam;
    nlohmann::json result = nlohmann::json::object();
    std::array<const char*, static_cast<std::size_t>(SysKey::Count)> names{};
#define LUMIN_RENDER_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                        \
    if (std::string_view(JSON_SECTION) == "render") {                                              \
        names[static_cast<std::size_t>(SysKey::ENUM)] = JSON_NAME;                                 \
        result[JSON_NAME] = {{"enabled", config.enabled(SysKey::ENUM)},                            \
                             {"params", nlohmann::json::object()}};                                \
    }
    LUMIN_CONFIG_KEY_TABLE(LUMIN_RENDER_KEY)
#undef LUMIN_RENDER_KEY
#define LUMIN_RENDER_PARAM(ENUM, OWNER, JSON_NAME, IS_VEC3, SCALAR, VX, VY, VZ)                    \
    if (const auto* owner = names[static_cast<std::size_t>(SysKey::OWNER)]) {                      \
        if constexpr (IS_VEC3) {                                                                   \
            const auto value = config.param3(SysParam::ENUM, glm::vec3(VX, VY, VZ));               \
            result[owner]["params"][JSON_NAME] = {value.x, value.y, value.z};                      \
        } else {                                                                                   \
            result[owner]["params"][JSON_NAME] = config.param(SysParam::ENUM, SCALAR);             \
        }                                                                                          \
    }
    LUMIN_CONFIG_PARAM_TABLE(LUMIN_RENDER_PARAM)
#undef LUMIN_RENDER_PARAM
    return result;
}

// Preset revision plus both override layers reconstruct every resolved graphics row.
// Serialize only at the report boundary, alongside renderer-observed values.
inline nlohmann::json graphics_configuration(const luminumbra::core::GraphicsSettings& settings,
                                             const luminumbra::core::ResolvedGraphics& resolved) {
    const auto overrides = [](const luminumbra::core::GraphicsOverrides& values) {
        auto result = nlohmann::json::object();
        for (const auto& [key, value] : values) {
            if (const auto* boolean = std::get_if<bool>(&value))
                result[key] = *boolean;
            else if (const auto* number = std::get_if<double>(&value))
                result[key] = *number;
            else
                result[key] = std::get<std::string>(value);
        }
        return result;
    };
    return {{"base_preset", luminumbra::core::GraphicsPresetName(resolved.base_preset)},
            {"preset_revision", resolved.preset_revision},
            {"saved_overrides", overrides(settings.overrides)},
            {"session_overrides", overrides(resolved.session_overrides)}};
}

// Startup may persist detected or migrated graphics settings before measurement.
// Accept only that owned write; every other content input must still match.
inline nlohmann::json content_after_settings_save(const nlohmann::json& before,
                                                  const nlohmann::json& after) {
    if (!after.contains("inputs/user_settings.json") ||
        !after.at("inputs/user_settings.json").is_string())
        throw std::runtime_error("saved graphics settings content is unavailable");
    auto previous_assets = before;
    auto current_assets = after;
    previous_assets.erase("inputs/user_settings.json");
    current_assets.erase("inputs/user_settings.json");
    if (previous_assets != current_assets)
        throw std::runtime_error("workload content changed during graphics settings save");
    return after;
}

// FNV-1a over a typed canonical stream, independent of JSON whitespace, locale,
// floating-point formatting, object insertion order and host endianness.
// Mirrors render_contract.render_identity. Called only at report boundaries.
inline std::string render_identity(const nlohmann::json& settings) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    std::uint64_t hash = 14695981039346656037ull;
    const auto byte = [&](unsigned char value) {
        hash = (hash ^ value) * 1099511628211ull;
    };
    const auto text = [&](std::string_view value) {
        for (const unsigned char item : value)
            byte(item);
    };
    const auto visit = [&](const auto& self, const nlohmann::json& value) -> void {
        if (value.is_object()) {
            byte('{');
            for (const auto& [key, item] : value.items()) {
                self(self, nlohmann::json(key));
                self(self, item);
            }
            byte('}');
        } else if (value.is_array()) {
            byte('[');
            for (const auto& item : value)
                self(self, item);
            byte(']');
        } else if (value.is_string()) {
            const auto& string = value.get_ref<const std::string&>();
            byte('s');
            text(std::to_string(string.size()));
            byte(':');
            text(string);
        } else if (value.is_boolean()) {
            byte(value.get<bool>() ? 't' : 'f');
        } else if (value.is_number_float()) {
            const double number = value.get<double>();
            if (!std::isfinite(number))
                throw std::invalid_argument("non-finite render configuration");
            byte('d');
            const auto bits = std::bit_cast<std::uint64_t>(number);
            for (unsigned shift = 0; shift < 64; shift += 8)
                byte(static_cast<unsigned char>(bits >> shift));
        } else if (value.is_number_integer()) {
            byte('i');
            text(value.dump());
            byte(';');
        } else if (value.is_null()) {
            byte('n');
        } else {
            throw std::invalid_argument("unsupported render configuration value");
        }
    };
    visit(visit, settings);
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4)
        result += digits[(hash >> shift) & 15u];
    return result;
}

} // namespace Luminumbra::Client::Measurement
