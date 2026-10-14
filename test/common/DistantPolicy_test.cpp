#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/world/DistantPolicy.h"
#include "nlohmann/json.hpp"

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {
using namespace Luminumbra::world;
using Json = nlohmann::json;
namespace fs = std::filesystem;

Json FullPolicy() {
    return {
        {"schema", "luminumbra.distant_policy.v1"},
        {"creature_species",
         {{"grovestrider",
           {{"coarse_need_rate_ppm", 0u},
            {"coarse_travel_m_per_cadence", 0u},
            {"promote_distance_m", 512u},
            {"lifespan_cadences", 0u},
            {"reproduce_cadences", 0u}}}}},
        {"plant_species", {{"wheat", {{"max_cadence_shift", 8u}, {"resume_cap_ticks", 36000u}}}}}};
}
std::string Document(const Json& value) {
    return value.dump() + "\n";
}
Json EmptyPolicy() {
    return {{"schema", "luminumbra.distant_policy.v1"},
            {"creature_species", Json::object()},
            {"plant_species", Json::object()}};
}
void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path;
}
#ifdef _WIN32
void RequireWindowsCall(bool ok, const char* operation) {
    if (!ok)
        throw std::runtime_error(std::string(operation) + ": Win32 error " +
                                 std::to_string(GetLastError()));
}
struct WindowsHandle {
    explicit WindowsHandle(HANDLE handle)
        : value(handle) {}
    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;
    ~WindowsHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    HANDLE value;
};
// An empty, protected DACL denies read access even when CI runs as the file owner.
// Restore the original DACL (including its inheritance state) before fixture cleanup.
class WindowsReadDenial {
public:
    explicit WindowsReadDenial(const fs::path& path)
        : m_path(path) {
        DWORD bytes = 0;
        const BOOL queried =
            GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
        RequireWindowsCall(queried == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
                               bytes != 0,
                           "query original DACL size");
        m_original.resize(bytes);
        RequireWindowsCall(
            GetFileSecurityW(
                path.c_str(), DACL_SECURITY_INFORMATION, m_original.data(), bytes, &bytes) != 0,
            "read original DACL");
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        RequireWindowsCall(GetSecurityDescriptorControl(m_original.data(), &control, &revision) !=
                               0,
                           "read DACL inheritance state");
        m_restoreFlags = DACL_SECURITY_INFORMATION | ((control & SE_DACL_PROTECTED) != 0
                                                          ? PROTECTED_DACL_SECURITY_INFORMATION
                                                          : UNPROTECTED_DACL_SECURITY_INFORMATION);
        ACL acl{};
        SECURITY_DESCRIPTOR descriptor{};
        RequireWindowsCall(
            InitializeAcl(&acl, static_cast<DWORD>(sizeof(acl)), ACL_REVISION) != 0 &&
                InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != 0 &&
                SetSecurityDescriptorDacl(&descriptor, TRUE, &acl, FALSE) != 0 &&
                SetFileSecurityW(path.c_str(),
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 &descriptor) != 0,
            "deny read access");
    }
    WindowsReadDenial(const WindowsReadDenial&) = delete;
    WindowsReadDenial& operator=(const WindowsReadDenial&) = delete;
    ~WindowsReadDenial() {
        EXPECT_NE(SetFileSecurityW(m_path.c_str(), m_restoreFlags, m_original.data()), 0)
            << "restore original DACL: " << GetLastError();
    }

private:
    fs::path m_path;
    std::vector<unsigned char> m_original;
    SECURITY_INFORMATION m_restoreFlags = 0;
};
void CreateJunction(const fs::path& link, const fs::path& target) {
    fs::create_directory(link);
    const WindowsHandle handle{
        CreateFileW(link.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr)};
    RequireWindowsCall(handle.value != INVALID_HANDLE_VALUE, "open junction directory");
    // Mount-point reparse layout. FSCTL_SET_REPARSE_POINT needs no symlink privilege.
    struct JunctionData {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD data_length = 0, reserved = 0;
        WORD substitute_offset = 0, substitute_length = 0;
        WORD print_offset = 0, print_length = 0;
        std::array<wchar_t, 4096> names{};
    } data;
    static_assert(offsetof(JunctionData, names) == 16);
    const auto print_name = fs::absolute(target).wstring();
    const auto substitute_name = std::wstring(L"\\??\\") + print_name;
    const auto name_units = substitute_name.size() + print_name.size() + 2;
    if (name_units > data.names.size())
        throw std::runtime_error("junction target exceeds test buffer");
    std::copy(substitute_name.begin(), substitute_name.end(), data.names.begin());
    std::copy(print_name.begin(),
              print_name.end(),
              data.names.begin() + static_cast<std::ptrdiff_t>(substitute_name.size() + 1));
    data.substitute_length = static_cast<WORD>(substitute_name.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute_name.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
    data.data_length = static_cast<WORD>(8 + name_units * sizeof(wchar_t));
    DWORD returned = 0;
    RequireWindowsCall(DeviceIoControl(handle.value,
                                       FSCTL_SET_REPARSE_POINT,
                                       &data,
                                       static_cast<DWORD>(data.data_length) + 8,
                                       nullptr,
                                       0,
                                       &returned,
                                       nullptr) != 0,
                       "create directory junction");
}
#endif
class ContentFixture {
public:
    ContentFixture() {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_content_");
        const auto source = fs::path(LUMINUMBRA_SOURCE_ROOT) / "data/common";
        const auto common = root / "data/common";
        fs::create_directories(common / "sim");
        for (const auto* domain : {"creatures", "foliage"}) {
            fs::create_directories(common / domain);
            fs::copy(source / domain / "species",
                     common / domain / "species",
                     fs::copy_options::recursive);
        }
    }
    ~ContentFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    [[nodiscard]] fs::path policy() const {
        return root / "data/common/sim/distant_policy.json";
    }
    [[nodiscard]] fs::path species(bool creature, std::string_view name) const {
        return root / "data/common" / (creature ? "creatures/species" : "foliage/species") /
               fs::u8path(std::string(name) + ".json");
    }
    fs::path root;
};
SimulationContent Admit(const ContentFixture& fixture) {
    SimulationContent content;
    std::string error;
    EXPECT_TRUE(AdmitSimulationContent(fixture.root, content, error)) << error;
    return content;
}
void ExpectSameRegistryOrder(const SimulationContent& actual, const SimulationContent& expected) {
    ASSERT_EQ(actual.creatures->size(), expected.creatures->size());
    ASSERT_EQ(actual.plants->size(), expected.plants->size());
    std::vector<std::string> biomes{"plains", "unknown"};
    for (std::size_t i = 0; i < expected.creatures->size(); ++i) {
        const auto& species = expected.creatures->all()[i];
        EXPECT_EQ(actual.creatures->all()[i].id, species.id);
        biomes.insert(biomes.end(), species.biomes.begin(), species.biomes.end());
    }
    for (std::size_t i = 0; i < expected.plants->size(); ++i)
        EXPECT_EQ(actual.plants->all()[i].id, expected.plants->all()[i].id);
    for (const auto& biome : biomes) {
        for (std::size_t pick = 0; pick < 2 * expected.creatures->size(); ++pick) {
            const auto* selected = actual.creatures->SelectForBiome(biome, pick);
            const auto* wanted = expected.creatures->SelectForBiome(biome, pick);
            if (!wanted) {
                EXPECT_EQ(selected, nullptr);
                continue;
            }
            ASSERT_NE(selected, nullptr);
            EXPECT_EQ(selected->id, wanted->id) << biome << ": " << pick;
        }
    }
}
void ExpectPolicyRefusal(std::string_view bytes, DistantPolicyOverrides& policy) {
    const auto before = policy;
    std::string error;
    EXPECT_FALSE(DistantPolicyOverrides::parse(bytes, policy, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(policy, before);
}

TEST(DistantPolicyTest, PolicyEveryIntegerEndpoint) {
    std::string error;
    DistantPolicyOverrides policy;
    const std::array<std::pair<const char*, std::uint64_t>, 5> fields{
        {{"coarse_need_rate_ppm", UINT32_MAX},
         {"coarse_travel_m_per_cadence", UINT32_MAX},
         {"promote_distance_m", UINT16_MAX},
         {"lifespan_cadences", UINT32_MAX},
         {"reproduce_cadences", UINT32_MAX}}};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& [name, maximum] = fields[index];
        for (const auto endpoint : {std::uint64_t{0}, maximum}) {
            auto input = FullPolicy();
            input["creature_species"]["grovestrider"][name] = endpoint;
            ASSERT_TRUE(DistantPolicyOverrides::parse(Document(input), policy, error)) << error;
            ASSERT_EQ(policy.creatures().size(), 1u);
            const auto& parsed = policy.creatures().at("grovestrider");
            const std::array<std::uint64_t, 5> actual{parsed.coarse_need_rate_ppm,
                                                      parsed.coarse_travel_m_per_cadence,
                                                      parsed.promote_distance_m,
                                                      parsed.lifespan_cadences,
                                                      parsed.reproduce_cadences};
            EXPECT_EQ(actual[index], endpoint);
        }
    }
    for (auto shift : {1u, 8u})
        for (auto cap : {0u, 36000u}) {
            auto input = FullPolicy();
            input["plant_species"]["wheat"] = {{"max_cadence_shift", shift},
                                               {"resume_cap_ticks", cap}};
            ASSERT_TRUE(DistantPolicyOverrides::parse(Document(input), policy, error)) << error;
            EXPECT_EQ(policy.plants().at("wheat").max_cadence_shift, shift);
            EXPECT_EQ(policy.plants().at("wheat").resume_cap_ticks, cap);
        }
}
TEST(DistantPolicyTest, PolicyBooleanDecimalExponentNegativeAndOverflowRefuse) {
    DistantPolicyOverrides policy;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    for (const auto* token : {"true",
                              "false",
                              "1.0",
                              "1e0",
                              "-1",
                              "-0",
                              "4294967296",
                              "18446744073709551616",
                              "\"1\"",
                              "null"}) {
        auto input = Document(FullPolicy());
        const auto at = input.find("\"coarse_need_rate_ppm\":0");
        ASSERT_NE(at, std::string::npos);
        input.replace(at + std::string("\"coarse_need_rate_ppm\":").size(), 1, token);
        ExpectPolicyRefusal(input, policy);
    }
    for (const auto& [field, value] :
         std::array<std::pair<const char*, unsigned>, 4>{{{"max_cadence_shift", 0u},
                                                          {"max_cadence_shift", 9u},
                                                          {"max_cadence_shift", 256u},
                                                          {"resume_cap_ticks", 36001u}}}) {
        auto input = FullPolicy();
        input["plant_species"]["wheat"][field] = value;
        ExpectPolicyRefusal(Document(input), policy);
    }
    auto input = FullPolicy();
    input["creature_species"]["grovestrider"]["promote_distance_m"] = 65536u;
    ExpectPolicyRefusal(Document(input), policy);
}
TEST(DistantPolicyTest, PolicyUnknownMissingDuplicateAndFutureFieldsRefuseAtomically) {
    DistantPolicyOverrides policy;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    for (const auto* name : {"schema", "creature_species", "plant_species"}) {
        auto input = FullPolicy();
        input.erase(name);
        ExpectPolicyRefusal(Document(input), policy);
    }
    for (const auto* field : {"coarse_need_rate_ppm",
                              "coarse_travel_m_per_cadence",
                              "promote_distance_m",
                              "lifespan_cadences",
                              "reproduce_cadences"}) {
        auto input = FullPolicy();
        input["creature_species"]["grovestrider"].erase(field);
        ExpectPolicyRefusal(Document(input), policy);
    }
    for (const auto* field : {"max_cadence_shift", "resume_cap_ticks"}) {
        auto input = FullPolicy();
        input["plant_species"]["wheat"].erase(field);
        ExpectPolicyRefusal(Document(input), policy);
    }
    auto input = FullPolicy();
    input["extra"] = 0;
    ExpectPolicyRefusal(Document(input), policy);
    input = FullPolicy();
    input["plant_species"]["wheat"]["extra"] = 0;
    ExpectPolicyRefusal(Document(input), policy);
    input = FullPolicy();
    input["creature_species"]["grovestrider"]["extra"] = 0;
    ExpectPolicyRefusal(Document(input), policy);
    input = FullPolicy();
    input["schema"] = "luminumbra.distant_policy.v2";
    ExpectPolicyRefusal(Document(input), policy);
    for (const auto* bytes :
         {"{\"schema\":\"luminumbra.distant_policy.v1\",\"schema\":\"luminumbra.distant_policy."
          "v1\",\"creature_species\":{},\"plant_species\":{}}\n",
          "{\"schema\":\"luminumbra.distant_policy.v1\",\"creature_species\":{},\"plant_species\":{"
          "\"wheat\":{\"max_cadence_shift\":8,\"max_cadence_shift\":8,\"resume_cap_ticks\":0}}}\n",
          "{\"schema\":\"luminumbra.distant_policy.v1\",\"creature_species\":{},\"plant_species\":{"
          "\"wheat\":{},\"wheat\":{}}}\n"})
        ExpectPolicyRefusal(bytes, policy);
}
TEST(DistantPolicyTest, PolicyUtf8BomCrFinalLfAndDepthRefuse) {
    DistantPolicyOverrides policy;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    const auto good = Document(FullPolicy());
    for (const auto& input :
         std::vector<std::string>{"\xef\xbb\xbf" + good,
                                  good + "\r\n",
                                  good.substr(0, good.size() - 1),
                                  std::string("\xc0\xaf") + good,
                                  std::string("\xed\xa0\x80") + good,
                                  std::string("\xf4\x90\x80\x80") + good,
                                  std::string(33, '[') + "0" + std::string(33, ']') + "\n"})
        ExpectPolicyRefusal(input, policy);
    auto input = FullPolicy();
    input["plant_species"][std::string("wheat\0hidden", 12)] = input["plant_species"]["wheat"];
    ExpectPolicyRefusal(Document(input), policy);
    EXPECT_FALSE(DistantPolicyOverrides::parse(
        std::string(33, '[') + "0" + std::string(33, ']') + "\n", policy, error));
    EXPECT_NE(error.find("depth"), std::string::npos);
    input = EmptyPolicy();
    input["plant_species"][std::string(40, '[')] = FullPolicy()["plant_species"]["wheat"];
    EXPECT_TRUE(DistantPolicyOverrides::parse(Document(input), policy, error)) << error;
    for (const auto& name : {std::string{}, std::string(257, 'a')}) {
        input = EmptyPolicy();
        input["plant_species"][name] = FullPolicy()["plant_species"]["wheat"];
        ExpectPolicyRefusal(Document(input), policy);
    }
    input = EmptyPolicy();
    input["plant_species"][std::string(256, 'a')] = FullPolicy()["plant_species"]["wheat"];
    EXPECT_TRUE(DistantPolicyOverrides::parse(Document(input), policy, error)) << error;
}
TEST(DistantPolicyTest, PolicyCleanAbsenceSelectsDefaults) {
    const ContentFixture fixture;
    DistantPolicyOverrides policy;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    EXPECT_EQ(DistantPolicyOverrides::load(fixture.policy(), policy, error),
              ContentReadStatus::Absent);
    EXPECT_TRUE(policy.creatures().empty());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(DistantPolicyOverrides::load(fixture.root / "missing/leaf.json", policy, error),
              ContentReadStatus::Absent);
    EXPECT_EQ(ReadSimulationDocument(fixture.policy(), false).status, ContentReadStatus::Error);
    fs::remove_all(fixture.root / "data/common/sim");
    SimulationContent candidate;
    EXPECT_TRUE(AdmitSimulationContent(fixture.root, candidate, error)) << error;
    EXPECT_TRUE(error.empty());
}
TEST(DistantPolicyTest, PolicyIoSymlinkNonregularOversizeAndChangedReadRefuse) {
    const ContentFixture fixture;
    DistantPolicyOverrides policy;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    const auto before = policy;
    const auto reject = [&](const fs::path& path, const ContentReadHook& hook = {}) {
        EXPECT_EQ(DistantPolicyOverrides::load(path, policy, error, hook),
                  ContentReadStatus::Error);
        EXPECT_EQ(policy, before);
        EXPECT_FALSE(error.empty());
    };
    reject(fixture.root);
    Write(fixture.policy(), std::string(kMaxContentDocumentBytes + 1, ' '));
    reject(fixture.policy());
    Write(fixture.policy(), Document(FullPolicy()));
    reject(fixture.policy(), [](const fs::path& path) {
        std::ofstream(path, std::ios::binary | std::ios::app) << ' ';
    });
    Write(fixture.policy(), Document(FullPolicy()));
    reject(fixture.policy(), [](const fs::path& path) {
        const auto stamp = fs::last_write_time(path);
        auto document = Document(FullPolicy());
        const auto at = document.find("\"coarse_need_rate_ppm\":0");
        ASSERT_NE(at, std::string::npos);
        document[at + std::string("\"coarse_need_rate_ppm\":").size()] = '1';
        Write(path, document);
        fs::last_write_time(path, stamp);
    });
    Write(fixture.policy(), Document(FullPolicy()));
    reject(fixture.policy(), [](const fs::path& path) { fs::remove(path); });
    Write(fixture.policy(), Document(FullPolicy()));
    reject(fixture.policy(),
           [](const fs::path&) { throw std::runtime_error("injected I/O failure"); });
}
#ifndef _WIN32
TEST(DistantPolicyTest, PolicyUnreadableModeRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    const auto original = fs::status(fixture.policy()).permissions();
    fs::permissions(fixture.policy(), fs::perms::none);
    const auto read = ReadSimulationDocument(fixture.policy());
    fs::permissions(fixture.policy(), original);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_FALSE(read.error.empty());
}
#endif

TEST(DistantPolicyTest, PolicyParentRemovedAfterCheckIsError) {
    const ContentFixture fixture;
    ContentReadBoundaries hooks;
    bool injected = false;
    hooks.after_parent_check = [&](const fs::path& path) {
        injected = fs::remove(path.parent_path());
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_FALSE(fs::exists(fixture.policy().parent_path()));
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_FALSE(read.error.empty());
    EXPECT_TRUE(read.bytes.empty());
}
TEST(DistantPolicyTest, PolicyParentReplacedAfterCheckIsError) {
    const ContentFixture fixture;
    ContentReadBoundaries hooks;
    bool injected = false;
    hooks.after_parent_check = [&](const fs::path& path) {
        fs::rename(path.parent_path(), path.parent_path().string() + "-old");
        injected = fs::create_directory(path.parent_path());
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_FALSE(read.error.empty());
}
#ifndef _WIN32
TEST(DistantPolicyTest, PolicyLeafSymlinkRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    const auto link = fixture.root / "leaf-link.json";
    std::error_code ec;
    fs::create_symlink(fixture.policy(), link, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(ReadSimulationDocument(link, true).status, ContentReadStatus::Error);
}
TEST(DistantPolicyTest, PolicyParentSymlinkRefuses) {
    const ContentFixture fixture;
    std::error_code ec;
    fs::create_directory_symlink(fixture.policy().parent_path(), fixture.root / "parent-link", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(ReadSimulationDocument(fixture.root / "parent-link/missing.json", true).status,
              ContentReadStatus::Error);
}
TEST(DistantPolicyTest, PolicyLeafReplacedBySymlinkBeforeOpenRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    const auto target = fixture.root / "replacement.json";
    Write(target, Document(FullPolicy()));
    const auto probe = fixture.root / "symlink-probe";
    std::error_code ec;
    fs::create_symlink(target, probe, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::remove(probe);
    bool injected = false;
    ContentReadBoundaries hooks;
    hooks.before_open = [&](const fs::path& path) {
        fs::remove(path);
        fs::create_symlink(target, path);
        injected = true;
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_FALSE(read.error.empty());
    EXPECT_TRUE(read.bytes.empty());
}
#else
TEST(DistantPolicyTest, PolicyWindowsReadAccessDeniedRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    {
        const WindowsReadDenial denial(fixture.policy());
        const WindowsHandle probe{
            CreateFileW(fixture.policy().c_str(),
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr)};
        const DWORD open_error = GetLastError();
        ASSERT_EQ(probe.value, INVALID_HANDLE_VALUE);
        EXPECT_EQ(open_error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
        const auto read = ReadSimulationDocument(fixture.policy(), true);
        EXPECT_EQ(read.status, ContentReadStatus::Error);
        EXPECT_FALSE(read.error.empty());
        EXPECT_TRUE(read.bytes.empty());
    }
    EXPECT_EQ(ReadSimulationDocument(fixture.policy()).status, ContentReadStatus::Found);
    fs::remove(fixture.policy());
    {
        const WindowsReadDenial denial(fixture.policy().parent_path());
        const auto read = ReadSimulationDocument(fixture.policy(), true);
        EXPECT_EQ(read.status, ContentReadStatus::Error);
        EXPECT_FALSE(read.error.empty());
    }
    EXPECT_EQ(ReadSimulationDocument(fixture.policy(), true).status, ContentReadStatus::Absent);
}
TEST(DistantPolicyTest, PolicyWindowsAccessDeniedBeforeOpenRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    std::unique_ptr<WindowsReadDenial> denial;
    ContentReadBoundaries hooks;
    hooks.before_open = [&](const fs::path& path) {
        denial = std::make_unique<WindowsReadDenial>(path);
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_NE(denial, nullptr);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_EQ(read.error, "content file open failed");
    EXPECT_TRUE(read.bytes.empty());
}
TEST(DistantPolicyTest, PolicyWindowsLeafJunctionRefuses) {
    const ContentFixture fixture;
    const auto link = fixture.root / "leaf-junction.json";
    CreateJunction(link, fixture.policy().parent_path());
    const auto read = ReadSimulationDocument(link, true);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_EQ(read.error, "content handle unreadable, nonregular, or reparse point");
    EXPECT_TRUE(fs::remove(link));
}
TEST(DistantPolicyTest, PolicyWindowsParentJunctionRefuses) {
    const ContentFixture fixture;
    const auto link = fixture.root / "parent-junction";
    fs::create_directory(fixture.policy().parent_path() / "nested");
    CreateJunction(link, fixture.policy().parent_path());
    for (const auto& path : {link / "missing.json", link / "nested/missing.json"}) {
        const auto read = ReadSimulationDocument(path, true);
        EXPECT_EQ(read.status, ContentReadStatus::Error);
        EXPECT_FALSE(read.error.empty());
    }
    EXPECT_TRUE(fs::remove(link));
}
TEST(DistantPolicyTest, PolicyWindowsLeafReplacedByJunctionBeforeOpenRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    bool injected = false;
    ContentReadBoundaries hooks;
    hooks.before_open = [&](const fs::path& path) {
        fs::remove(path);
        CreateJunction(path, path.parent_path());
        injected = true;
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_EQ(read.error, "content handle unreadable, nonregular, or reparse point");
    EXPECT_TRUE(read.bytes.empty());
    // Remove the junction itself before recursive fixture cleanup (its target is its parent).
    EXPECT_TRUE(fs::remove(fixture.policy()));
}
TEST(DistantPolicyTest, PolicyWindowsParentReplacedByJunctionAfterCheckRefuses) {
    const ContentFixture fixture;
    bool injected = false;
    ContentReadBoundaries hooks;
    hooks.after_parent_check = [&](const fs::path& path) {
        const auto original = path.parent_path().parent_path() / "sim-original";
        fs::rename(path.parent_path(), original);
        CreateJunction(path.parent_path(), original);
        injected = true;
    };
    const auto read = ReadSimulationDocument(fixture.policy(), true, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_FALSE(read.error.empty());
    EXPECT_TRUE(fs::remove(fixture.policy().parent_path()));
}
#endif
TEST(DistantPolicyTest, PolicyRegularLeafReplacedBeforeOpenRefuses) {
    const ContentFixture fixture;
    Write(fixture.policy(), Document(FullPolicy()));
    bool injected = false;
    ContentReadBoundaries hooks;
    hooks.before_open = [&](const fs::path& path) {
        const auto old = path.parent_path() / "original.json";
        const auto stamp = fs::last_write_time(path);
        fs::rename(path, old);
        fs::copy_file(old, path);
        fs::last_write_time(path, stamp);
        injected = true;
    };
    const auto read = ReadSimulationDocument(fixture.policy(), false, {}, hooks);
    EXPECT_TRUE(injected);
    EXPECT_EQ(read.status, ContentReadStatus::Error);
    EXPECT_EQ(read.error, "content replaced before open");
    EXPECT_TRUE(read.bytes.empty());
}
TEST(DistantPolicyTest, PolicyGrownAndShrunkDuringReadRefuse) {
    const ContentFixture fixture;
    for (const std::uintmax_t changed_size : {1024u, 16384u}) {
        SCOPED_TRACE(changed_size);
        Write(fixture.policy(), std::string(8192, ' '));
        bool injected = false;
        ContentReadBoundaries hooks;
        hooks.during_read = [&](const fs::path& path) {
            fs::resize_file(path, changed_size);
            injected = true;
        };
        const auto read = ReadSimulationDocument(fixture.policy(), false, {}, hooks);
        EXPECT_TRUE(injected);
        EXPECT_EQ(read.status, ContentReadStatus::Error);
        EXPECT_FALSE(read.error.empty());
        EXPECT_TRUE(read.bytes.empty());
    }
}

TEST(DistantPolicyTest, PolicyAbsentEmptyAndExplicitDefaultsResolveEqually) {
    const ContentFixture fixture;
    const auto absent = Admit(fixture);
    ASSERT_EQ(absent.creatures->size(), 10u);
    ASSERT_EQ(absent.plants->size(), 6u);
    Write(fixture.policy(), Document(EmptyPolicy()));
    const auto empty = Admit(fixture);
    Write(fixture.policy(), Document(FullPolicy()));
    const auto explicit_defaults = Admit(fixture);
    EXPECT_EQ(absent.policy, empty.policy);
    EXPECT_EQ(absent.policy, explicit_defaults.policy);
    EXPECT_EQ(absent.identity, empty.identity);
    EXPECT_EQ(absent.identity, explicit_defaults.identity);
    EXPECT_EQ(absent.policy.creatures.at("grovestrider"), kDefaultCreaturePolicy);
    EXPECT_EQ(absent.policy.plants.at("wheat"), kDefaultPlantPolicy);
    EXPECT_EQ(absent.policy.defaults_version, 1u);
}
TEST(DistantPolicyTest, PolicyUnknownSpeciesAndInventoryCountRefuse) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto before = content.identity;
    std::string error;
    for (const bool creature : {false, true}) {
        auto input = FullPolicy();
        const auto* domain = creature ? "creature_species" : "plant_species";
        const auto* known = creature ? "grovestrider" : "wheat";
        input[domain]["unknown"] = input[domain][known];
        Write(fixture.policy(), Document(input));
        EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
        EXPECT_EQ(content.identity, before);
    }
    auto excessive = EmptyPolicy();
    for (unsigned i = 0; i <= content.plant_definitions.size(); ++i)
        excessive["plant_species"]["unknown-" + std::to_string(i)] =
            FullPolicy()["plant_species"]["wheat"];
    Write(fixture.policy(), Document(excessive));
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
    EXPECT_NE(error.find("entries exceed resolved inventory"), std::string::npos);
    fs::remove(fixture.policy());
    auto definitions = content.creature_definitions;
    while (definitions.size() <= kMaxSpeciesDefinitions)
        definitions.push_back(definitions.front());
    ResolvedDistantPolicy resolved;
    EXPECT_FALSE(ResolveDistantPolicy({}, definitions, content.plant_definitions, resolved, error));
    auto input = EmptyPolicy();
    for (unsigned i = 0; i <= kMaxSpeciesDefinitions; ++i)
        input["plant_species"]["species_" + std::to_string(i)] =
            FullPolicy()["plant_species"]["wheat"];
    DistantPolicyOverrides overrides;
    EXPECT_FALSE(DistantPolicyOverrides::parse(Document(input), overrides, error));
    fs::remove_all(fixture.root / "data/common/creatures/species");
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
    EXPECT_EQ(content.identity, before);
}
TEST(DistantPolicyTest, CompleteInventoryCollisionAfterZeroRemapRefuses) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto before = content.identity;
    std::string error;
    for (const bool creature : {true, false}) {
        for (const auto* name : {"vector_7999", "vector_67098"})
            Write(fixture.species(creature, name), Document(Json{{"id", name}}));
        EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
        EXPECT_NE(error.find("collision"), std::string::npos);
        EXPECT_EQ(content.identity, before);
        for (const auto* name : {"vector_7999", "vector_67098"})
            fs::remove(fixture.species(creature, name));
    }
    Write(fixture.species(true, "vector_7999"), "{\"id\":\"vector_7999\"}\n");
    Write(fixture.species(false, "vector_67098"), "{\"id\":\"vector_67098\"}\n");
    ASSERT_TRUE(AdmitSimulationContent(fixture.root, content, error)) << error;
    EXPECT_NE(content.identity, before);
}
TEST(DistantPolicyTest, SpeciesIdentityCrossLanguageVectors) {
    for (const auto& [name, expected] : std::array<std::pair<const char*, std::uint16_t>, 4>{
             {{"grovestrider", 56165}, {"wheat", 7403}, {"vector_7999", 1}, {"vector_67098", 1}}}) {
        EXPECT_EQ(SimulationSpeciesId(name), expected);
        EXPECT_EQ(luminumbra::foliage::SpeciesId16(name), expected);
        EXPECT_EQ(Luminumbra::Components::CreatureSpeciesId16(name), expected);
    }
    EXPECT_THROW((void)SimulationSpeciesId(""), std::invalid_argument);
    EXPECT_THROW((void)SimulationSpeciesId(std::string_view("bad\0name", 8)),
                 std::invalid_argument);
    EXPECT_EQ(SimulationContentDigest(""), 14695981039346656037ull);
    EXPECT_EQ(SimulationContentDigest("hello"), 0xa430d84680aabd0bull);
}
TEST(DistantPolicyTest, DefinitionMalformedPresentFieldNeverDefaults) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto before = content.identity;
    std::string error;
    const std::vector<Json> creature_bad{{{"predator", 0}},
                                         {{"nocturnal", "true"}},
                                         {{"rarity", 1.1}},
                                         {{"rarity", 1e39}},
                                         {{"rarity", nullptr}},
                                         {{"base_color", {0.1, 0.2}}},
                                         {{"base_color", {0.1, 0.2, -0.1}}},
                                         {{"display_name", 12}},
                                         {{"schema", "luminumbra.creature.species.v2"}},
                                         {{"unknown", 0}},
                                         {{"brain", {{"sleep_weight", "bad"}}}},
                                         {{"brain", {{"sleep_weight", 1e39}}}},
                                         {{"brain", {{"unknown", 0}}}},
                                         {{"genome_ranges", {{"vigilance", {2, 1}}}}},
                                         {{"genome_ranges", {{"unknown", {0, 1}}}}},
                                         {{"biomes", {"plains", "plains"}}}};
    const std::vector<Json> plant_bad{{{"perennial", 1}},
                                      {{"render_archetype", false}},
                                      {{"lifespan_ticks", -1}},
                                      {{"lifespan_ticks", 1.5}},
                                      {{"lifespan_ticks", true}},
                                      {{"lifespan_ticks", std::uint64_t{1} << 32}},
                                      {{"unknown", 0}},
                                      {{"genes", {{"GrowthRate", {-0.1, 1}}}}},
                                      {{"genes", {{"Yield", {1, 0}}}}},
                                      {{"genes", {{"Hardiness", {0, 1, 2}}}}},
                                      {{"genes", {{"unknown", {0, 1}}}}}};
    for (const bool creature : {true, false}) {
        const auto* name = creature ? "grovestrider" : "wheat";
        const auto& definitions =
            creature ? content.creature_definitions : content.plant_definitions;
        const auto original =
            std::find_if(definitions.begin(), definitions.end(), [name](const auto& entry) {
                return entry.name == name;
            })->bytes;
        for (const auto& patch : creature ? creature_bad : plant_bad) {
            Json definition{{"id", name}};
            definition.update(patch);
            Write(fixture.species(creature, name), Document(definition));
            EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error)) << patch;
            EXPECT_EQ(content.identity, before);
        }
        Write(fixture.species(creature, name),
              "{\"id\":\"" + std::string(name) + "\",\"id\":\"" + name + "\"}\n");
        EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
        Write(fixture.species(creature, name),
              Document(Json{{"id", creature ? "gloomstalker" : "oak"}}));
        EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
        Write(fixture.species(creature, name), original);
    }
    // Every optional field may be absent: validated legacy parsers keep their own compiled
    // defaults.
    Write(fixture.species(true, "grovestrider"), "{\"id\":\"grovestrider\"}\n");
    Write(fixture.species(false, "wheat"), "{\"id\":\"wheat\"}\n");
    ASSERT_TRUE(AdmitSimulationContent(fixture.root, content, error)) << error;
    EXPECT_FLOAT_EQ(content.creatures->FindByName("grovestrider")->rarity, 0.5f);
    EXPECT_EQ(content.plants->Find("wheat")->lifespan_ticks, 1200u);
}
TEST(DistantPolicyTest, IdentityExcludesPresentationWhitespacePathsAndJsonOrder) {
    const ContentFixture fixture;
    const auto content = Admit(fixture);
    for (const auto& definition : content.creature_definitions) {
        auto root = Json::parse(definition.bytes);
        root["display_name"] = "New display name";
        root["base_color"] = {0.1, 0.2, 0.3};
        root["rarity"] = 0.7;
        Write(fixture.species(true, definition.name), root.dump(4) + "\n");
    }
    for (const auto& definition : content.plant_definitions) {
        auto root = Json::parse(definition.bytes);
        root["render_archetype"] = "other_appearance";
        Write(fixture.species(false, definition.name), root.dump(2) + "\n");
    }
    EXPECT_EQ(Admit(fixture).identity, content.identity);
    // Definitions are identified by their names, independently of file paths.
    fs::rename(fixture.species(true, "grovestrider"), fixture.species(true, "renamed"));
    fs::rename(fixture.species(false, "wheat"), fixture.species(false, "first"));
    const auto renamed = Admit(fixture);
    EXPECT_EQ(renamed.identity, content.identity);
    ASSERT_NO_FATAL_FAILURE(ExpectSameRegistryOrder(renamed, content));
    auto reversed = content;
    for (auto& definition : reversed.creature_definitions) {
        auto root = Json::parse(definition.bytes);
        if (root.contains("biomes"))
            std::reverse(root["biomes"].begin(), root["biomes"].end());
        definition.bytes = root.dump();
    }
    EXPECT_EQ(BuildSimulationContentIdentity(reversed), content.identity);
}
TEST(DistantPolicyTest, IdentityIncludesUnreferencedCreatureAndPlantSimulationDefinitions) {
    const ContentFixture fixture;
    const auto content = Admit(fixture);
    for (const bool creature : {true, false}) {
        const auto& definitions =
            creature ? content.creature_definitions : content.plant_definitions;
        auto changed = content;
        auto& target = creature ? changed.creature_definitions : changed.plant_definitions;
        auto root = Json::parse(definitions.front().bytes);
        if (creature)
            root["brain"]["wander_weight"] = 0.71;
        else
            root["lifespan_ticks"] = 54321;
        target.front().bytes = root.dump();
        const auto identity = BuildSimulationContentIdentity(changed);
        EXPECT_NE(identity, content.identity);
        EXPECT_EQ(identity.effective_policy_digest, content.identity.effective_policy_digest);
    }
}
TEST(DistantPolicyTest, IdentityIncludesDefaultAndAlgorithmVersionsAndPopulationConfiguration) {
    const ContentFixture fixture;
    const auto content = Admit(fixture);
    auto changed = content;
    changed.policy.defaults_version = 2;
    EXPECT_NE(BuildSimulationContentIdentity(changed).effective_policy_digest,
              content.identity.effective_policy_digest);
    changed = content;
    changed.policy.creatures.at("grovestrider").coarse_need_rate_ppm = 1000;
    EXPECT_NE(BuildSimulationContentIdentity(changed).effective_policy_digest,
              content.identity.effective_policy_digest);
    changed.identity.algorithm_version = 2;
    EXPECT_NE(changed.identity.encode(), content.identity.encode());
    EXPECT_NE(BuildSimulationContentIdentity(content, "registered-populator:v1").population_digest,
              content.identity.population_digest);
    EXPECT_NE(BuildSimulationContentIdentity(content, kDefaultPopulationContract, "config:v1")
                  .population_digest,
              content.identity.population_digest);
    // Framed callback/configuration cannot alias at their boundary.
    EXPECT_NE(BuildSimulationContentIdentity(content, "ab", "c").population_digest,
              BuildSimulationContentIdentity(content, "a", "bc").population_digest);
}
TEST(DistantPolicyTest, IdentityIndependentOfDirectoryEnumeration) {
    const ContentFixture fixture;
    const std::string unicode_name = "\xc3\xa9-species";
    Write(fixture.species(true, unicode_name), Document(Json{{"id", unicode_name}}));
    EXPECT_TRUE(ContentNameLess{}("z", unicode_name));
    EXPECT_FALSE(ContentNameLess{}(unicode_name, "z"));
    const auto content = Admit(fixture);
    ASSERT_EQ(content.creature_definitions.back().name, unicode_name);
    auto reversed = content;
    std::reverse(reversed.creature_definitions.begin(), reversed.creature_definitions.end());
    std::reverse(reversed.plant_definitions.begin(), reversed.plant_definitions.end());
    EXPECT_EQ(BuildSimulationContentIdentity(reversed), content.identity);
    for (const bool creature : {true, false}) {
        const auto& entries = creature ? content.creature_definitions : content.plant_definitions;
        for (const auto& entry : entries)
            fs::remove(fixture.species(creature, entry.name));
        std::size_t index = 0;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            Write(fixture.species(creature, "entry-" + std::to_string(index++)), it->bytes);
    }
    const auto reordered = Admit(fixture);
    EXPECT_EQ(reordered.identity, content.identity);
    ASSERT_NO_FATAL_FAILURE(ExpectSameRegistryOrder(reordered, content));
    EXPECT_TRUE(std::is_sorted(reordered.creature_definitions.begin(),
                               reordered.creature_definitions.end(),
                               [](const auto& left, const auto& right) {
                                   return ContentNameLess{}(left.name, right.name);
                               }));
    EXPECT_TRUE(std::is_sorted(reordered.plant_definitions.begin(),
                               reordered.plant_definitions.end(),
                               [](const auto& left, const auto& right) {
                                   return ContentNameLess{}(left.name, right.name);
                               }));
}
TEST(DistantPolicyTest, DefinitionResourceCapsAndDirectoryChangesRefuseAtomically) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto before = content.identity;
    std::string error;
    for (unsigned i = 0; i < kMaxSpeciesDefinitions; ++i) {
        const auto name = "bounded_" + std::to_string(i);
        Write(fixture.species(true, name), Document(Json{{"id", name}}));
    }
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
    EXPECT_NE(error.find("256"), std::string::npos);
    EXPECT_EQ(content.identity, before);
    for (unsigned i = 0; i < kMaxSpeciesDefinitions; ++i)
        fs::remove(fixture.species(true, "bounded_" + std::to_string(i)));
    for (unsigned i = 0; i < 17; ++i) {
        const auto name = "large_" + std::to_string(i);
        auto bytes = Document(Json{{"id", name}});
        bytes.resize(kMaxContentDocumentBytes - 1, ' ');
        bytes += '\n';
        Write(fixture.species(true, name), bytes);
    }
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
    EXPECT_NE(error.find("byte cap"), std::string::npos);
    EXPECT_EQ(content.identity, before);
    for (unsigned i = 0; i < 17; ++i)
        fs::remove(fixture.species(true, "large_" + std::to_string(i)));
    bool injected = false;
    EXPECT_FALSE(AdmitSimulationContent(
        fixture.root, content, error, kDefaultPopulationContract, {}, [&](const fs::path& path) {
            if (!injected && path.parent_path().filename() == "species") {
                injected = true;
                Write(fixture.species(true, "new_definition"), "{\"id\":\"new_definition\"}\n");
            }
        }));
    EXPECT_TRUE(injected);
    EXPECT_NE(error.find("directory changed"), std::string::npos);
    EXPECT_EQ(content.identity, before);
}
TEST(DistantPolicyTest, OverrideCanonicalRoundTripAndSemanticDefaults) {
    DistantPolicyOverrides policy, restored;
    std::string error;
    ASSERT_TRUE(DistantPolicyOverrides::parse(Document(FullPolicy()), policy, error));
    ASSERT_TRUE(DistantPolicyOverrides::parse(policy.canonical_bytes(), restored, error));
    EXPECT_EQ(policy, restored);
    EXPECT_EQ(policy.canonical_bytes(), restored.canonical_bytes());
    EXPECT_EQ(kDefaultCreaturePolicy.coarse_need_rate_ppm, 0u);
    EXPECT_EQ(kDefaultCreaturePolicy.coarse_travel_m_per_cadence, 0u);
    EXPECT_EQ(kDefaultCreaturePolicy.lifespan_cadences, 0u);
    EXPECT_EQ(kDefaultCreaturePolicy.reproduce_cadences, 0u);
    EXPECT_EQ(kDefaultCreaturePolicy.promote_distance_m, 512u);
}
TEST(DistantPolicyTest, DuplicateInventoryNamesRefuseAcrossDifferentPaths) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto before = content.identity;
    Write(fixture.species(true, "duplicate"), "{\"id\":\"grovestrider\"}\n");
    std::string error;
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error));
    EXPECT_NE(error.find("duplicate"), std::string::npos);
    EXPECT_EQ(content.identity, before);
}
TEST(DistantPolicyTest, ResolvedSpeciesDefaultsEqualExplicitSimulationDefaults) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    Write(fixture.species(true, "grovestrider"), "{\"id\":\"grovestrider\"}\n");
    const auto implicit = Admit(fixture);
    const luminumbra::ai::CreatureSpecies defaults;
    const auto& b = defaults.brain;
    Json definition{{"id", "grovestrider"},
                    {"predator", false},
                    {"nocturnal", false},
                    {"brain",
                     {{"wander_weight", b.wander_weight},
                      {"rest_weight", b.rest_weight},
                      {"sleep_weight", b.sleep_weight},
                      {"hunt_weight", b.hunt_weight},
                      {"flee_weight", b.flee_weight},
                      {"graze_weight", b.graze_weight},
                      {"flee_logistic_m", b.flee_logistic_m},
                      {"flee_logistic_c", b.flee_logistic_c}}}};
    Write(fixture.species(true, "grovestrider"), Document(definition));
    EXPECT_EQ(Admit(fixture).identity, implicit.identity);
}
TEST(DistantPolicyTest, PopulationIdentityInputsAreAdoptedAndBoundedAtomically) {
    const ContentFixture fixture;
    auto content = Admit(fixture);
    const auto original = content.identity;
    std::string error;
    ASSERT_TRUE(
        AdmitSimulationContent(fixture.root, content, error, "fixture-populator:v1", "config:v1"))
        << error;
    EXPECT_NE(content.identity.population_digest, original.population_digest);
    EXPECT_EQ(content.identity.effective_policy_digest, original.effective_policy_digest);
    const auto before = content.identity;
    EXPECT_FALSE(AdmitSimulationContent(fixture.root, content, error, "", "config:v1"));
    EXPECT_EQ(content.identity, before);
    EXPECT_FALSE(AdmitSimulationContent(fixture.root,
                                        content,
                                        error,
                                        "fixture-populator:v1",
                                        std::string(kMaxContentDocumentBytes + 1, 'x')));
    EXPECT_EQ(content.identity, before);
}
} // namespace
