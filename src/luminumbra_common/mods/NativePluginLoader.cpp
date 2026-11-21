#include "NativePluginLoader.h"

#include "authoring/PrefabDigest.h"
#include "core/FilesystemPath.h"
#include "core/NativeFileOwner.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace Luminumbra::Mods {
namespace {
using Filesystem::NativeFileOwner;
// Limit one load to 64 MiB of owned binary bytes and 32767 native path characters.
constexpr std::uint64_t kMaxPluginBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxPath = 32767;

#if defined(_WIN32)
std::filesystem::path FinalPath(HANDLE file) {
    std::wstring buffer(kMaxPath + 1, L'\0');
    const auto length = GetFinalPathNameByHandleW(
        file, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool LockParents(const std::filesystem::path& path, std::vector<NativeFileOwner>& locks) {
    // Parse the handle's extended DOS/UNC spelling without libstdc++ root-name
    // interpretation, which differs between MSVC and older MinGW versions.
    const auto& spelling = path.native();
    std::size_t root_length = 0;
    if (spelling.starts_with(L"\\\\?\\UNC\\")) {
        const auto server_end = spelling.find(L'\\', 8);
        if (server_end == std::wstring::npos) {
            return false;
        }
        const auto share_end = spelling.find(L'\\', server_end + 1);
        if (share_end == std::wstring::npos) {
            return false;
        }
        root_length = share_end + 1;
    } else if (spelling.starts_with(L"\\\\?\\") && spelling.size() >= 7 && spelling[5] == L':' &&
               spelling[6] == L'\\') {
        root_length = 7;
    } else {
        return false;
    }
    auto parent = spelling.substr(0, std::max(root_length, spelling.find_last_of(L'\\')));
    // A pathological directory chain is refused before more than 256 opens.
    for (std::size_t depth = 0; depth < 256; ++depth) {
        NativeFileOwner directory(
            CreateFileW(parent.c_str(),
                        FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(directory.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            FinalPath(directory.get()).native() != parent) {
            return false;
        }
        locks.push_back(std::move(directory));
        if (parent.size() == root_length) {
            return true;
        }
        parent.resize(std::max(root_length, parent.find_last_of(L'\\')));
    }
    return false;
}
#endif
} // namespace

struct NativePluginLoader::State {
    std::optional<NativeFileOwner> image;
#if defined(_WIN32)
    std::vector<NativeFileOwner> parents;
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    LumHostApi host{};
    LumPluginShutdownFn stop = nullptr;
    void* context = nullptr;
    bool initialized = false;

    ~State() {
        shutdown();
        if (library != nullptr) {
#if defined(_WIN32)
            FreeLibrary(library);
#else
            dlclose(library);
#endif
        }
    }

    void shutdown() noexcept {
        if (initialized) {
            initialized = false;
            stop(context);
            context = nullptr;
        }
    }

    template<typename Function>
    Function symbol(const char* name) const {
#if defined(_WIN32)
        return std::bit_cast<Function>(GetProcAddress(library, name));
#else
        return reinterpret_cast<Function>(dlsym(library, name));
#endif
    }

    NativePluginResult open(const std::filesystem::path& path, std::string_view pin) {
        const auto& native = path.native();
        if (native.empty() || native.size() > kMaxPath ||
            native.find(std::filesystem::path::value_type{}) != native.npos) {
            return NativePluginResult::FileRefused;
        }
        std::error_code error;
        const auto absolute = Filesystem::AbsolutePath(path, error);
        if (error || absolute.native().size() > kMaxPath) {
            return NativePluginResult::FileRefused;
        }
#if defined(_WIN32)
        NativeFileOwner input(CreateFileW(absolute.c_str(),
                                          GENERIC_READ,
                                          FILE_SHARE_READ,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER length{};
        if (GetFileType(input.get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(input.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !GetFileSizeEx(input.get(), &length) || length.QuadPart <= 0 ||
            static_cast<std::uint64_t>(length.QuadPart) > kMaxPluginBytes) {
            return NativePluginResult::FileRefused;
        }
        const auto resolved = FinalPath(input.get());
        if (resolved.empty() || !LockParents(resolved, parents) ||
            FinalPath(input.get()) != resolved) {
            return NativePluginResult::FileRefused;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.QuadPart));
        DWORD count = 0;
        if (!ReadFile(
                input.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
            count != bytes.size()) {
            return NativePluginResult::FileRefused;
        }
#else
        NativeFileOwner input(::open(absolute.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        struct stat info {};
        if (fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            static_cast<std::uint64_t>(info.st_size) > kMaxPluginBytes) {
            return NativePluginResult::FileRefused;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::uint8_t extra = 0;
        if (::read(input.get(), bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()) ||
            ::read(input.get(), &extra, 1) != 0) {
            return NativePluginResult::FileRefused;
        }
#endif
        if (Authoring::Detail::Sha256(bytes) != pin) {
            return NativePluginResult::HashMismatch;
        }
#if defined(_WIN32)
        image.emplace(input.release());
        // File sharing denies writes/deletion; locked parents keep this handle's
        // resolved spelling attached to the same image through LoadLibraryExW.
        library = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        NativeFileOwner snapshot(
            memfd_create("luminumbra-plugin", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (snapshot.get() < 0 ||
            ::write(snapshot.get(), bytes.data(), bytes.size()) !=
                static_cast<ssize_t>(bytes.size()) ||
            fcntl(snapshot.get(),
                  F_ADD_SEALS,
                  F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
            return NativePluginResult::FileRefused;
        }
        // Keep the sealed descriptor alive as long as the module. The dynamic
        // linker consumes exactly the verified bytes, never the input path again.
        const auto descriptor_path = "/proc/self/fd/" + std::to_string(snapshot.get());
        image.emplace(snapshot.release());
        library = dlopen(descriptor_path.c_str(), RTLD_LOCAL | RTLD_NOW);
#endif
        return library != nullptr ? NativePluginResult::Loaded : NativePluginResult::LoadFailed;
    }
};

NativePluginLoader::NativePluginLoader() = default;
NativePluginLoader::~NativePluginLoader() = default;

NativePluginResult NativePluginLoader::load(const std::filesystem::path& path,
                                            std::string_view expected_sha256,
                                            bool trust_granted,
                                            const LumHostApi& host) {
    if (!trust_granted) {
        return NativePluginResult::Untrusted;
    }
    if (m_state) {
        return NativePluginResult::AlreadyLoaded;
    }
    if (host.struct_size < sizeof(LumHostApi) || host.abi_major != LUM_ABI_MAJOR ||
        host.abi_minor != LUM_ABI_MINOR || host.get_proc_address == nullptr) {
        return NativePluginResult::InvalidHost;
    }
    if (expected_sha256.size() != 64 ||
        !std::all_of(expected_sha256.begin(), expected_sha256.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        })) {
        return NativePluginResult::InvalidPin;
    }
    auto candidate = std::make_unique<State>();
    const auto result = candidate->open(path, expected_sha256);
    if (result != NativePluginResult::Loaded) {
        return result;
    }
    const auto query = candidate->symbol<LumPluginQueryFn>("luminumbra_plugin_query");
    const auto start = candidate->symbol<LumPluginInitFn>("luminumbra_plugin_init");
    candidate->stop = candidate->symbol<LumPluginShutdownFn>("luminumbra_plugin_shutdown");
    if (query == nullptr || start == nullptr || candidate->stop == nullptr) {
        return NativePluginResult::MissingSymbol;
    }
    LumPluginInfo info{};
    info.struct_size = sizeof(info);
    if (!query(&info)) {
        return NativePluginResult::QueryFailed;
    }
    if (info.struct_size < sizeof(info) || info.required_abi_major != LUM_ABI_MAJOR ||
        info.min_abi_minor > LUM_ABI_MINOR) {
        return NativePluginResult::IncompatibleAbi;
    }
    candidate->host = host;
    if (start(&candidate->host, &candidate->context) != LUM_STATUS_OK) {
        if (candidate->context != nullptr) {
            candidate->stop(candidate->context);
            candidate->context = nullptr;
        }
        return NativePluginResult::InitFailed;
    }
    candidate->initialized = true;
    m_state = std::move(candidate);
    return NativePluginResult::Loaded;
}

void NativePluginLoader::shutdown() noexcept {
    if (m_state) {
        m_state->shutdown();
    }
}

bool NativePluginLoader::active() const noexcept {
    return m_state && m_state->initialized;
}

} // namespace Luminumbra::Mods
