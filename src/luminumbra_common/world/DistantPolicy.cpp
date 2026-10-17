#include "DistantPolicy.h"

#include "../ai/CreatureSpeciesRegistry.h"
#include "../core/FilesystemPath.h"
#include "../foliage/SpeciesRegistry.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Luminumbra::world {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}
void Require(bool condition, const std::string& message) {
    if (!condition)
        throw std::invalid_argument(message);
}
void Append(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void Frame(std::string& bytes, std::string_view value) {
    Require(value.size() <= std::numeric_limits<std::uint32_t>::max(), "content framing overflow");
    Append(bytes, value.size(), 4);
    bytes.append(value);
}
bool ValidUtf8(std::string_view bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[at++]);
        if (first < 128)
            continue;
        unsigned remaining = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            code = first & 31u;
            minimum = 128;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            code = first & 15u;
            minimum = 2048;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            code = first & 7u;
            minimum = 65536;
        } else {
            return false;
        }
        if (remaining > bytes.size() - at)
            return false;
        for (unsigned i = 0; i < remaining; ++i) {
            const auto next = static_cast<unsigned char>(bytes[at++]);
            if ((next & 0xc0u) != 0x80u)
                return false;
            code = (code << 6) | (next & 63u);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
void CheckDepth(std::string_view bytes) {
    std::size_t depth = 0;
    bool quoted = false, escaped = false;
    for (char byte : bytes) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && byte == '\\') {
            escaped = true;
            continue;
        }
        if (byte == '"') {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (byte == '{' || byte == '[') {
            ++depth;
            Require(depth <= kMaxContentDepth, "content parser depth exceeds 32");
        } else if (byte == '}' || byte == ']') {
            Require(depth != 0, "unbalanced content JSON");
            --depth;
        }
    }
}
Json StrictJson(std::string_view bytes, bool authored_document = true) {
    Require(!bytes.empty() && bytes.size() <= kMaxContentDocumentBytes, "content byte limit");
    // The lexer treats a raw NUL as end-of-input, even with bounded iterators.
    Require(bytes.find('\0') == std::string_view::npos, "content contains raw NUL");
    Require(!authored_document ||
                (bytes.back() == '\n' && bytes.find('\r') == std::string_view::npos),
            "content requires LF only and final LF");
    Require(!bytes.starts_with("\xef\xbb\xbf") && ValidUtf8(bytes),
            "content requires UTF-8 without BOM");
    CheckDepth(bytes);
    std::vector<std::set<std::string>> keys;
    std::size_t nodes = 0;
    const auto callback = [&keys, &nodes](int, Json::parse_event_t event, Json& value) {
        Require(++nodes <= 65536, "content node limit");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        if (value.is_string()) {
            const auto& string = value.get_ref<const std::string&>();
            Require(string.size() <= 256 && string.find('\0') == std::string::npos,
                    "content string limit or embedded NUL");
        }
        if (event == Json::parse_event_t::key) {
            Require(keys.back().size() < 256, "content object key limit");
            Require(keys.back().insert(value.get<std::string>()).second, "duplicate content key");
        }
        if (event == Json::parse_event_t::array_end)
            Require(value.size() <= 256, "content array count limit");
        return true;
    };
    return Json::parse(bytes.begin(), bytes.end(), callback);
}
void Keys(const Json& value,
          std::initializer_list<std::string_view> allowed,
          std::initializer_list<std::string_view> required = {}) {
    Require(value.is_object(), "content block must be an object");
    for (const auto& entry : value.items())
        Require(std::find(allowed.begin(), allowed.end(), entry.key()) != allowed.end(),
                "unknown content key");
    for (auto name : required)
        Require(value.contains(std::string(name)), "missing required content key");
}
std::uint64_t Integer(const Json& value, std::uint64_t low, std::uint64_t high) {
    // Positive integer tokens are unsigned in nlohmann; signed zero rejects the token -0.
    Require(value.is_number_unsigned(), "content requires an unsigned integer token");
    const auto number = value.get<std::uint64_t>();
    Require(number >= low && number <= high, "content integer out of range");
    return number;
}
std::string Name(const Json& value) {
    Require(value.is_string(), "content requires a string");
    const auto name = value.get<std::string>();
    Require(!name.empty() && name.size() <= 256 && name.find('\0') == std::string::npos &&
                ValidUtf8(name),
            "invalid content name");
    return name;
}
double Number(const Json& value, float low, float high) {
    Require(value.is_number(), "content requires a finite number");
    // Refuse outside the finite float domain before a legacy parser narrows it.
    const auto number = value.get<double>();
    Require(std::isfinite(number) && number >= low && number <= high,
            "content number out of range");
    return number;
}
void OptionalString(const Json& object, const char* key) {
    if (object.contains(key))
        (void)Name(object.at(key));
}
void OptionalBoolean(const Json& object, const char* key) {
    if (object.contains(key))
        Require(object.at(key).is_boolean(), "content requires a boolean");
}
void OptionalNumber(const Json& object, const char* key, float low, float high) {
    if (object.contains(key))
        (void)Number(object.at(key), low, high);
}
void Range(const Json& value, float low, float high) {
    Require(value.is_array() && value.size() == 2, "gene/climate range requires two numbers");
    Require(Number(value[0], low, high) <= Number(value[1], low, high), "inverted content range");
}
void StringList(const Json& values,
                const SpeciesNames* inventory = nullptr,
                bool nonempty = false) {
    Require(values.is_array() && (!nonempty || !values.empty()) && values.size() <= 256,
            "invalid content string list");
    SpeciesNames seen;
    for (const auto& value : values) {
        const auto name = Name(value);
        Require(seen.insert(name).second && (!inventory || inventory->contains(name)),
                "duplicate or unknown content reference");
    }
}
void CreatureDefinition(const Json& root) {
    Keys(root,
         {"schema",
          "id",
          "display_name",
          "predator",
          "nocturnal",
          "rarity",
          "base_color",
          "biomes",
          "brain",
          "genome_ranges"},
         {"id"});
    (void)Name(root.at("id"));
    if (root.contains("schema"))
        Require(root.at("schema") == "luminumbra.creature.species.v1",
                "unsupported creature schema");
    OptionalString(root, "display_name");
    OptionalBoolean(root, "predator");
    OptionalBoolean(root, "nocturnal");
    OptionalNumber(root, "rarity", 0, 1);
    if (root.contains("base_color")) {
        const auto& color = root.at("base_color");
        Require(color.is_array() && color.size() == 3, "base color requires three numbers");
        for (const auto& channel : color)
            (void)Number(channel, 0, 1);
    }
    if (root.contains("biomes"))
        StringList(root.at("biomes"));
    if (root.contains("brain")) {
        const auto& brain = root.at("brain");
        Keys(brain,
             {"wander_weight",
              "rest_weight",
              "sleep_weight",
              "hunt_weight",
              "flee_weight",
              "graze_weight",
              "flee_logistic_m",
              "flee_logistic_c"});
        for (const auto& entry : brain.items())
            (void)Number(entry.value(),
                         -std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max());
    }
    if (root.contains("genome_ranges")) {
        const auto& genes = root.at("genome_ranges");
        Keys(genes,
             {"move_speed",
              "vigilance",
              "hunger_threshold",
              "size_scale",
              "vision_cos_half_fov",
              "vision_range",
              "hearing_range"});
        for (const auto& entry : genes.items())
            Range(entry.value(),
                  -std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max());
    }
}
void PlantDefinition(const Json& root) {
    Keys(root, {"id", "render_archetype", "perennial", "lifespan_ticks", "genes"}, {"id"});
    (void)Name(root.at("id"));
    OptionalString(root, "render_archetype");
    OptionalBoolean(root, "perennial");
    if (root.contains("lifespan_ticks"))
        (void)Integer(root.at("lifespan_ticks"), 0, std::numeric_limits<std::uint32_t>::max());
    if (root.contains("genes")) {
        const auto& genes = root.at("genes");
        Keys(genes,
             {"GrowthRate",
              "MaxScale",
              "DroughtTolerance",
              "ColdTolerance",
              "HeatTolerance",
              "LeafDensity",
              "Yield",
              "Hardiness"});
        for (const auto& entry : genes.items())
            Range(entry.value(), 0, 1);
    }
}
bool Readable(const fs::file_status& status) {
    constexpr auto reads = fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read;
    return status.permissions() == fs::perms::unknown ||
           (status.permissions() & reads) != fs::perms::none;
}
void CheckDirectoryComponent(const fs::path& path) {
#ifdef _WIN32
    // Junctions may look like ordinary directories to std::filesystem.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
            "content parent missing, unreadable, or reparse point");
#endif
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    Require(!ec && fs::is_directory(status) && !fs::is_symlink(status) && Readable(status),
            "content parent missing, unreadable, or symlink");
}
void CheckDirectory(const fs::path& path) {
    const auto absolute = Filesystem::AbsolutePath(path);
#ifdef _WIN32
    // Walk native prefixes: a server name or device prefix alone is not a
    // directory. MinGW's filesystem root decomposition can also lose UNC roots.
    const auto& native = absolute.native();
    auto root_end = absolute.root_path().native().size();
    if (Filesystem::IsWindowsUncPath(absolute)) {
        const bool extended_unc =
            native.size() >= 8 && native[2] == L'?' && (native[4] == L'U' || native[4] == L'u') &&
            (native[5] == L'N' || native[5] == L'n') && (native[6] == L'C' || native[6] == L'c') &&
            (native[7] == L'\\' || native[7] == L'/');
        const auto server_end = native.find_first_of(L"\\/", extended_unc ? 8 : 2);
        Require(server_end != std::wstring::npos, "content directory root is incomplete");
        const auto share_end = native.find_first_of(L"\\/", server_end + 1);
        root_end = share_end == std::wstring::npos ? native.size() : share_end + 1;
    }
    CheckDirectoryComponent(fs::path(native.substr(0, root_end)));
    for (auto start = root_end; start < native.size();) {
        const auto separator = native.find_first_of(L"\\/", start);
        const auto end = separator == std::wstring::npos ? native.size() : separator;
        CheckDirectoryComponent(fs::path(native.substr(0, end)));
        start = end == native.size() ? end : end + 1;
    }
#else
    fs::path current = absolute.root_path();
    CheckDirectoryComponent(current);
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        CheckDirectoryComponent(current);
    }
#endif
}
// All native opening stays here: pin the parent, compare the checked leaf with the
// opened handle, and read/verify through that handle without a second path open.
ContentReadResult OpenSimulationDocument(const fs::path& path,
                                         bool optional,
                                         const ContentReadHook& after_read,
                                         const ContentReadBoundaries& boundaries) {
    struct FileInfo {
        std::uint64_t volume = 0, index = 0, size = 0, modified = 0;
        bool directory = false;
    };
#ifdef _WIN32
    struct NativeHandle {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~NativeHandle() {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    };
    const auto open_path = [](const fs::path& target, DWORD access) {
        return CreateFileW(target.c_str(),
                           access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    };
    const auto handle_info = [](HANDLE handle) {
        BY_HANDLE_FILE_INFORMATION info{};
        Require(GetFileInformationByHandle(handle, &info) != 0 &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                    GetFileType(handle) == FILE_TYPE_DISK,
                "content handle unreadable, nonregular, or reparse point");
        return FileInfo{info.dwVolumeSerialNumber,
                        (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
                        (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
                        (std::uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) |
                            info.ftLastWriteTime.dwLowDateTime,
                        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
    };
    const auto open_parent = [&] {
        return open_path(path.parent_path(), GENERIC_READ);
    };
    const auto path_info = [&](const fs::path& target, FileInfo& info) {
        const NativeHandle query{open_path(target, FILE_READ_ATTRIBUTES)};
        if (query.value == INVALID_HANDLE_VALUE) {
            Require(GetLastError() == ERROR_FILE_NOT_FOUND ||
                        GetLastError() == ERROR_PATH_NOT_FOUND,
                    "content path query failed");
            return false;
        }
        info = handle_info(query.value);
        Require(Readable(fs::symlink_status(target)), "content path unreadable");
        return true;
    };
#else
    struct NativeHandle {
        int value = -1;
        ~NativeHandle() {
            if (value >= 0)
                ::close(value);
        }
    };
    const auto stat_info = [](const struct stat& info) {
        constexpr auto reads = S_IRUSR | S_IRGRP | S_IROTH;
        Require((S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) && (info.st_mode & reads) != 0 &&
                    info.st_size >= 0,
                "content handle unreadable, nonregular, or symlink");
        return FileInfo{static_cast<std::uint64_t>(info.st_dev),
                        static_cast<std::uint64_t>(info.st_ino),
                        static_cast<std::uint64_t>(info.st_size),
                        static_cast<std::uint64_t>(info.st_mtime),
                        S_ISDIR(info.st_mode)};
    };
    const auto handle_info = [&](int handle) {
        struct stat info {};
        Require(::fstat(handle, &info) == 0, "content handle query failed");
        return stat_info(info);
    };
    const auto open_parent = [&] {
        return ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    };
    const auto path_info = [&](const fs::path& target, FileInfo& info) {
        struct stat status {};
        if (::lstat(target.c_str(), &status) != 0) {
            Require(errno == ENOENT, "content path query failed");
            return false;
        }
        info = stat_info(status);
        return true;
    };
#endif
    const auto same_identity = [](const FileInfo& left, const FileInfo& right) {
        return left.volume == right.volume && left.index == right.index &&
               left.directory == right.directory;
    };
    CheckDirectory(path.parent_path());
    FileInfo parent_before;
    Require(path_info(path.parent_path(), parent_before) && parent_before.directory,
            "content parent missing or non-directory");
    const NativeHandle parent{open_parent()};
    Require(same_identity(parent_before, handle_info(parent.value)), "content parent changed");
    const auto validate_parent = [&] {
        CheckDirectory(path.parent_path());
        FileInfo current;
        Require(path_info(path.parent_path(), current) && current.directory &&
                    same_identity(parent_before, current),
                "content parent changed or missing");
        const NativeHandle readable_parent{open_parent()};
        Require(same_identity(parent_before, handle_info(readable_parent.value)),
                "content parent changed or unreadable");
    };
    if (boundaries.after_parent_check)
        boundaries.after_parent_check(path);
    FileInfo before;
    if (!path_info(path, before)) {
        validate_parent();
        Require(optional, "missing required content file");
        return {ContentReadStatus::Absent, {}, {}};
    }
    Require(!before.directory && before.size <= kMaxContentDocumentBytes,
            "content file nonregular or exceeds byte limit");
    if (boundaries.before_open)
        boundaries.before_open(path);
#ifdef _WIN32
    const NativeHandle file{open_path(path, GENERIC_READ)};
    Require(file.value != INVALID_HANDLE_VALUE, "content file open failed");
    const auto read_chunk = [&](char* data, std::size_t count) {
        DWORD received = 0;
        Require(ReadFile(file.value, data, static_cast<DWORD>(count), &received, nullptr) != 0,
                "content read failed");
        return static_cast<std::size_t>(received);
    };
    const auto rewind_file = [&] {
        const LARGE_INTEGER start{};
        Require(SetFilePointerEx(file.value, start, nullptr, FILE_BEGIN) != 0,
                "content rewind failed");
    };
#else
    const NativeHandle file{::openat(
        parent.value, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
    Require(file.value >= 0, "content file open failed");
    const auto read_chunk = [&](char* data, std::size_t count) {
        ssize_t received = 0;
        do {
            received = ::read(file.value, data, count);
        } while (received < 0 && errno == EINTR);
        Require(received >= 0, "content read failed");
        return static_cast<std::size_t>(received);
    };
    const auto rewind_file = [&] {
        Require(::lseek(file.value, 0, SEEK_SET) == 0, "content rewind failed");
    };
#endif
    const auto unchanged = [&](const FileInfo& current) {
        return same_identity(before, current) && before.size == current.size &&
               before.modified == current.modified;
    };
    Require(unchanged(handle_info(file.value)), "content replaced before open");
    validate_parent();
    std::string bytes(static_cast<std::size_t>(before.size), '\0');
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto count =
            read_chunk(bytes.data() + at, std::min<std::size_t>(4096, bytes.size() - at));
        Require(count != 0, "content read failed or size changed");
        at += count;
        if (boundaries.during_read && at == count)
            boundaries.during_read(path);
    }
    std::array<char, 4096> scratch{};
    Require(read_chunk(scratch.data(), 1) == 0 && unchanged(handle_info(file.value)),
            "content size changed during read");
    if (after_read)
        after_read(path);
    Require(unchanged(handle_info(file.value)), "content changed after read");
    // Compare exact bytes too: a rewrite may preserve the file size and timestamp.
    rewind_file();
    at = 0;
    while (at < bytes.size()) {
        const auto count = read_chunk(scratch.data(), std::min(scratch.size(), bytes.size() - at));
        Require(count != 0 && bytes.compare(at, count, scratch.data(), count) == 0,
                "content changed during read");
        at += count;
    }
    Require(read_chunk(scratch.data(), 1) == 0 && unchanged(handle_info(file.value)),
            "content changed during verification");
    validate_parent();
    FileInfo final_info;
    Require(path_info(path, final_info) && unchanged(final_info),
            "content path changed during read");
    return {ContentReadStatus::Found, std::move(bytes), {}};
}

std::string Utf8Stem(const fs::path& path) {
    return Filesystem::GenericUtf8PathString(path.stem());
}
std::vector<fs::path> DefinitionFiles(const fs::path& directory) {
    CheckDirectory(directory);
    std::vector<fs::path> files;
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        // Bound actual enumeration, including files excluded by the extension filter.
        Require(++entries <= kMaxSpeciesDirectoryEntries, "species directory exceeds 512 entries");
        if (entry.path().extension() != ".json")
            continue;
        Require(files.size() < kMaxSpeciesDefinitions, "species inventory exceeds 256 definitions");
        files.push_back(entry.path());
    }
    Require(!files.empty(), "required species inventory is empty");
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return ContentNameLess{}(Utf8Stem(left), Utf8Stem(right));
    });
    return files;
}
std::vector<CapturedSpeciesDefinition> CaptureDefinitions(const fs::path& directory,
                                                          bool creature,
                                                          std::size_t& total,
                                                          const ContentReadHook& after_read) {
    const auto files = DefinitionFiles(directory);
    std::uintmax_t planned_bytes = total;
    for (const auto& path : files) {
        const auto size = fs::file_size(path);
        Require(size <= kMaxContentDocumentBytes &&
                    size <= kMaxCapturedDefinitionBytes - planned_bytes,
                "captured definition byte cap");
        planned_bytes += size;
    }
    std::vector<CapturedSpeciesDefinition> captured;
    SpeciesNames names;
    std::set<std::uint16_t> ids;
    for (const auto& path : files) {
        Require(fs::file_size(path) <= kMaxCapturedDefinitionBytes - total,
                "captured definition byte cap");
        auto read = ReadSimulationDocument(path, false, after_read);
        Require(read.status == ContentReadStatus::Found, read.error);
        Require(read.bytes.size() <= kMaxCapturedDefinitionBytes - total,
                "captured definition byte cap");
        total += read.bytes.size();
        const auto root = StrictJson(read.bytes, false);
        const auto name = Name(root.at("id"));
        if (creature)
            CreatureDefinition(root);
        else
            PlantDefinition(root);
        const auto id =
            creature ? SimulationSpeciesId(name) : luminumbra::foliage::SpeciesId16(name);
        Require(names.insert(name).second && ids.insert(id).second,
                "duplicate species name or id collision");
        captured.push_back({name, id, std::move(read.bytes)});
    }
    Require(DefinitionFiles(directory) == files, "species directory changed during capture");
    return captured;
}
void ResolveDomain(const std::vector<CapturedSpeciesDefinition>& definitions,
                   SpeciesNames& names,
                   bool creature) {
    Require(!definitions.empty() && definitions.size() <= kMaxSpeciesDefinitions,
            "invalid species inventory count");
    std::set<std::uint16_t> ids;
    for (const auto& definition : definitions) {
        const auto name = Name(Json(definition.name));
        const auto id =
            creature ? SimulationSpeciesId(name) : luminumbra::foliage::SpeciesId16(name);
        Require(definition.species_id == id && names.insert(name).second &&
                    ids.insert(definition.species_id).second,
                "duplicate species name or id collision");
    }
}
void FloatBits(std::string& bytes, float value) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    Require(std::isfinite(value), "non-finite simulation species value");
    Append(bytes, std::bit_cast<std::uint32_t>(value), 4);
}
std::string SimulationDefinition(const CapturedSpeciesDefinition& definition, bool creature) {
    const auto root = StrictJson(definition.bytes, false);
    Require(Name(root.at("id")) == definition.name, "captured species name mismatch");
    std::string bytes;
    std::string error;
    if (creature) {
        CreatureDefinition(root);
        luminumbra::ai::CreatureSpecies species;
        Require(luminumbra::ai::ParseCreatureSpecies(root, species, error), error);
        Require(species.species_id() == definition.species_id, "captured creature id mismatch");
        Append(bytes, species.predator, 1);
        Append(bytes, species.nocturnal, 1);
        std::sort(species.biomes.begin(), species.biomes.end(), ContentNameLess{});
        Append(bytes, species.biomes.size(), 4);
        for (const auto& biome : species.biomes)
            Frame(bytes, biome);
        const auto& brain = species.brain;
        for (const auto value : {brain.wander_weight,
                                 brain.rest_weight,
                                 brain.sleep_weight,
                                 brain.hunt_weight,
                                 brain.flee_weight,
                                 brain.graze_weight,
                                 brain.flee_logistic_m,
                                 brain.flee_logistic_c,
                                 brain.drink_weight})
            FloatBits(bytes, value);
        for (const auto& bounds : species.genome_ranges.core) {
            FloatBits(bytes, bounds.lo);
            FloatBits(bytes, bounds.hi);
        }
        for (const auto& bounds : species.genome_ranges.sensory) {
            FloatBits(bytes, bounds.lo);
            FloatBits(bytes, bounds.hi);
        }
    } else {
        PlantDefinition(root);
        luminumbra::foliage::SpeciesTemplate species;
        Require(luminumbra::foliage::ParseSpeciesTemplate(root, species, error), error);
        Require(luminumbra::foliage::SpeciesId16(species.id) == definition.species_id,
                "captured plant id mismatch");
        Append(bytes, species.perennial, 1);
        Append(bytes, species.lifespan_ticks, 4);
        for (std::size_t i = 0; i < species.gene_lo.size(); ++i) {
            FloatBits(bytes, species.gene_lo[i]);
            FloatBits(bytes, species.gene_hi[i]);
        }
    }
    return bytes;
}
std::uint64_t DefinitionDigest(std::string_view domain,
                               const std::vector<CapturedSpeciesDefinition>& definitions,
                               bool creature) {
    std::vector<const CapturedSpeciesDefinition*> sorted;
    for (const auto& definition : definitions)
        sorted.push_back(&definition);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return ContentNameLess{}(left->name, right->name);
    });
    std::string bytes(domain);
    Append(bytes, sorted.size(), 4);
    for (const auto* definition : sorted) {
        Frame(bytes, definition->name);
        Append(bytes, definition->species_id, 2);
        Frame(bytes, SimulationDefinition(*definition, creature));
    }
    return SimulationContentDigest(bytes);
}
CreatureDistantPolicy ParseCreaturePolicy(const Json& value) {
    Keys(value,
         {"coarse_need_rate_ppm",
          "coarse_travel_m_per_cadence",
          "promote_distance_m",
          "lifespan_cadences",
          "reproduce_cadences"},
         {"coarse_need_rate_ppm",
          "coarse_travel_m_per_cadence",
          "promote_distance_m",
          "lifespan_cadences",
          "reproduce_cadences"});
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    return {
        static_cast<std::uint32_t>(Integer(value.at("coarse_need_rate_ppm"), 0, maximum)),
        static_cast<std::uint32_t>(Integer(value.at("coarse_travel_m_per_cadence"), 0, maximum)),
        static_cast<std::uint16_t>(Integer(value.at("promote_distance_m"), 0, 65535)),
        static_cast<std::uint32_t>(Integer(value.at("lifespan_cadences"), 0, maximum)),
        static_cast<std::uint32_t>(Integer(value.at("reproduce_cadences"), 0, maximum))};
}
} // namespace

bool ContentNameLess::operator()(std::string_view left, std::string_view right) const noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](unsigned char a, unsigned char b) {
            return a < b;
        });
}
std::uint16_t SimulationSpeciesId(std::string_view name) {
    Require(!name.empty() && name.size() <= 256 && ValidUtf8(name) &&
                name.find('\0') == std::string_view::npos,
            "invalid species name");
    const std::string terminated(name);
    return Luminumbra::Components::CreatureSpeciesId16(terminated.c_str());
}
std::uint64_t SimulationContentDigest(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}
std::string BindSimulationContentIdentity(std::string_view configuration_identity,
                                          const SimulationContentIdentity& content) {
    std::string bytes = "simulation-session:v1:";
    Frame(bytes, configuration_identity);
    bytes += content.encode();
    auto hash = SimulationContentDigest(bytes);
    std::string hex(16, '0');
    for (std::size_t index = 0; index < hex.size(); ++index) {
        hex[hex.size() - 1 - index] = "0123456789abcdef"[hash & 15u];
        hash >>= 4u;
    }
    return hex;
}
ContentReadResult ReadSimulationDocument(const fs::path& path,
                                         bool optional,
                                         const ContentReadHook& after_read,
                                         const ContentReadBoundaries& boundaries) {
    try {
        return OpenSimulationDocument(path, optional, after_read, boundaries);
    } catch (const std::exception& exception) {
        return {ContentReadStatus::Error, {}, exception.what()};
    }
}

bool DistantPolicyOverrides::parse(std::string_view bytes,
                                   DistantPolicyOverrides& out,
                                   std::string& error) {
    error.clear();
    try {
        const auto root = StrictJson(bytes);
        Keys(root,
             {"schema", "creature_species", "plant_species"},
             {"schema", "creature_species", "plant_species"});
        Require(root.at("schema") == "luminumbra.distant_policy.v1",
                "unsupported distant policy schema");
        const auto& creatures = root.at("creature_species");
        const auto& plants = root.at("plant_species");
        Require(creatures.is_object() && plants.is_object() &&
                    creatures.size() <= kMaxSpeciesDefinitions &&
                    plants.size() <= kMaxSpeciesDefinitions,
                "invalid policy inventory count");
        DistantPolicyOverrides candidate;
        for (const auto& entry : creatures.items())
            candidate.m_creatures.emplace(Name(Json(entry.key())),
                                          ParseCreaturePolicy(entry.value()));
        for (const auto& entry : plants.items()) {
            Keys(entry.value(),
                 {"max_cadence_shift", "resume_cap_ticks"},
                 {"max_cadence_shift", "resume_cap_ticks"});
            candidate.m_plants.emplace(
                Name(Json(entry.key())),
                PlantDistantPolicy{
                    static_cast<std::uint8_t>(Integer(entry.value().at("max_cadence_shift"), 1, 8)),
                    static_cast<std::uint32_t>(
                        Integer(entry.value().at("resume_cap_ticks"), 0, 36000))});
        }
        out = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid distant policy: ") + exception.what();
        return false;
    }
}
ContentReadStatus DistantPolicyOverrides::load(const fs::path& path,
                                               DistantPolicyOverrides& out,
                                               std::string& error,
                                               const ContentReadHook& after_read) {
    const auto read = ReadSimulationDocument(path, true, after_read);
    error = read.error;
    if (read.status == ContentReadStatus::Absent)
        out = {};
    if (read.status == ContentReadStatus::Found && !parse(read.bytes, out, error))
        return ContentReadStatus::Error;
    return read.status;
}
std::string DistantPolicyOverrides::canonical_bytes() const {
    Json root{{"schema", "luminumbra.distant_policy.v1"},
              {"creature_species", Json::object()},
              {"plant_species", Json::object()}};
    for (const auto& [name, policy] : m_creatures)
        root["creature_species"][name] = {
            {"coarse_need_rate_ppm", policy.coarse_need_rate_ppm},
            {"coarse_travel_m_per_cadence", policy.coarse_travel_m_per_cadence},
            {"promote_distance_m", policy.promote_distance_m},
            {"lifespan_cadences", policy.lifespan_cadences},
            {"reproduce_cadences", policy.reproduce_cadences}};
    for (const auto& [name, policy] : m_plants)
        root["plant_species"][name] = {{"max_cadence_shift", policy.max_cadence_shift},
                                       {"resume_cap_ticks", policy.resume_cap_ticks}};
    return root.dump() + "\n";
}
bool ResolveDistantPolicy(const DistantPolicyOverrides& overrides,
                          const std::vector<CapturedSpeciesDefinition>& creatures,
                          const std::vector<CapturedSpeciesDefinition>& plants,
                          ResolvedDistantPolicy& out,
                          std::string& error) {
    error.clear();
    try {
        SpeciesNames creature_names, plant_names;
        ResolveDomain(creatures, creature_names, true);
        ResolveDomain(plants, plant_names, false);
        Require(overrides.creatures().size() <= creatures.size() &&
                    overrides.plants().size() <= plants.size(),
                "policy entries exceed resolved inventory");
        ResolvedDistantPolicy candidate;
        for (const auto& definition : creatures)
            candidate.creatures.emplace(definition.name, kDefaultCreaturePolicy);
        for (const auto& definition : plants)
            candidate.plants.emplace(definition.name, kDefaultPlantPolicy);
        for (const auto& [name, policy] : overrides.creatures()) {
            Require(creature_names.contains(name), "unknown creature policy species");
            candidate.creatures.at(name) = policy;
        }
        for (const auto& [name, policy] : overrides.plants()) {
            Require(plant_names.contains(name), "unknown plant policy species");
            candidate.plants.at(name) = policy;
        }
        for (const auto& definition : plants)
            candidate.plant_ids.emplace(definition.species_id,
                                        candidate.plants.at(definition.name));
        out = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
const PlantDistantPolicy& ResolvedDistantPolicy::plant(std::uint16_t species_id) const {
    // Unspecified legacy plants retain the same single compiled policy; unknown nonzero IDs refuse.
    if (species_id == 0)
        return kDefaultPlantPolicy;
    return plant_ids.at(species_id);
}
std::string ResolvedDistantPolicy::canonical_bytes() const {
    std::string bytes = "ds-effective-policy:v1";
    Append(bytes, defaults_version, 2);
    Append(bytes, kDistantAlgorithmVersion, 2);
    // Seal the compiled fallback table even when every species has an override.
    Append(bytes, kDefaultCreaturePolicy.coarse_need_rate_ppm, 4);
    Append(bytes, kDefaultCreaturePolicy.coarse_travel_m_per_cadence, 4);
    Append(bytes, kDefaultCreaturePolicy.promote_distance_m, 2);
    Append(bytes, kDefaultCreaturePolicy.lifespan_cadences, 4);
    Append(bytes, kDefaultCreaturePolicy.reproduce_cadences, 4);
    Append(bytes, kDefaultPlantPolicy.max_cadence_shift, 1);
    Append(bytes, kDefaultPlantPolicy.resume_cap_ticks, 4);
    Append(bytes, creatures.size(), 4);
    for (const auto& [name, policy] : creatures) {
        Frame(bytes, name);
        Append(bytes, SimulationSpeciesId(name), 2);
        Append(bytes, policy.coarse_need_rate_ppm, 4);
        Append(bytes, policy.coarse_travel_m_per_cadence, 4);
        Append(bytes, policy.promote_distance_m, 2);
        Append(bytes, policy.lifespan_cadences, 4);
        Append(bytes, policy.reproduce_cadences, 4);
    }
    Append(bytes, plants.size(), 4);
    for (const auto& [name, policy] : plants) {
        Frame(bytes, name);
        Append(bytes, SimulationSpeciesId(name), 2);
        Append(bytes, policy.max_cadence_shift, 1);
        Append(bytes, policy.resume_cap_ticks, 4);
    }
    return bytes;
}
std::string SimulationContentIdentity::encode() const {
    std::string bytes;
    Append(bytes, encoding_version, 2);
    Append(bytes, defaults_version, 2);
    Append(bytes, algorithm_version, 2);
    Append(bytes, 0, 2);
    for (auto digest : {effective_policy_digest,
                        creature_definitions_digest,
                        plant_definitions_digest,
                        population_digest})
        Append(bytes, digest, 8);
    return bytes;
}
bool SimulationContentIdentity::decode(std::string_view bytes, SimulationContentIdentity& out) {
    if (bytes.size() != kEncodedBytes)
        return false;
    std::size_t at = 0;
    const auto read = [&bytes, &at](unsigned width) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at++])) << (8 * i);
        return value;
    };
    SimulationContentIdentity candidate;
    candidate.encoding_version = static_cast<std::uint16_t>(read(2));
    candidate.defaults_version = static_cast<std::uint16_t>(read(2));
    candidate.algorithm_version = static_cast<std::uint16_t>(read(2));
    if (candidate.encoding_version != 1 || candidate.defaults_version != kDistantDefaultsVersion ||
        candidate.algorithm_version != kDistantAlgorithmVersion || read(2) != 0)
        return false;
    candidate.effective_policy_digest = read(8);
    candidate.creature_definitions_digest = read(8);
    candidate.plant_definitions_digest = read(8);
    candidate.population_digest = read(8);
    out = candidate;
    return true;
}
SimulationContentIdentity
BuildSimulationContentIdentity(const SimulationContent& content,
                               std::string_view callback_contract,
                               std::string_view population_configuration) {
    std::string population = "ds-population:v1";
    Frame(population, callback_contract);
    Frame(population, population_configuration);
    SimulationContentIdentity identity;
    identity.defaults_version = content.policy.defaults_version;
    identity.effective_policy_digest = SimulationContentDigest(content.policy.canonical_bytes());
    identity.creature_definitions_digest =
        DefinitionDigest("ds-creature-definitions:v1", content.creature_definitions, true);
    identity.plant_definitions_digest =
        DefinitionDigest("ds-plant-definitions:v1", content.plant_definitions, false);
    identity.population_digest = SimulationContentDigest(population);
    return identity;
}
bool AdmitSimulationContent(const fs::path& runtime_root,
                            SimulationContent& out,
                            std::string& error,
                            std::string_view callback_contract,
                            std::string_view population_configuration,
                            const ContentReadHook& after_read) {
    error.clear();
    try {
        (void)Name(Json(callback_contract));
        Require(population_configuration.size() <= kMaxContentDocumentBytes,
                "population configuration byte limit");
        const auto common = runtime_root / "data/common";
        SimulationContent candidate;
        std::size_t total = 0;
        candidate.creature_definitions =
            CaptureDefinitions(common / "creatures/species", true, total, after_read);
        candidate.plant_definitions =
            CaptureDefinitions(common / "foliage/species", false, total, after_read);
        const auto by_name = [](const auto& left, const auto& right) {
            return ContentNameLess{}(left.name, right.name);
        };
        std::sort(
            candidate.creature_definitions.begin(), candidate.creature_definitions.end(), by_name);
        std::sort(candidate.plant_definitions.begin(), candidate.plant_definitions.end(), by_name);
        DistantPolicyOverrides overrides;
        Require(DistantPolicyOverrides::load(
                    common / "sim/distant_policy.json", overrides, error, after_read) !=
                    ContentReadStatus::Error,
                error);
        Require(ResolveDistantPolicy(overrides,
                                     candidate.creature_definitions,
                                     candidate.plant_definitions,
                                     candidate.policy,
                                     error),
                error);
        auto creatures = std::make_shared<luminumbra::ai::CreatureSpeciesRegistry>();
        auto plants = std::make_shared<luminumbra::foliage::SpeciesRegistry>();
        for (const auto& definition : candidate.creature_definitions)
            Require(creatures->AddFromJsonText(definition.bytes, error),
                    "validated creature registry parse failed");
        for (const auto& definition : candidate.plant_definitions)
            Require(plants->AddFromJsonText(definition.bytes, error),
                    "validated plant registry parse failed");
        candidate.creatures = std::move(creatures);
        candidate.plants = std::move(plants);
        candidate.identity =
            BuildSimulationContentIdentity(candidate, callback_contract, population_configuration);
        out = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Simulation content admission failed: ") + exception.what();
        return false;
    }
}
} // namespace Luminumbra::world
