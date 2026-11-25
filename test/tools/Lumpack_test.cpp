#include "../../tools/lumpack/Lumpack.h"
#include "../support/UniqueTempPath.h"

#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/mods/ModManifest.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32) && !defined(__CYGWIN__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace paths = Luminumbra::Filesystem;
using nlohmann::json;

struct Tree {
    const fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("lumpack-");
    ~Tree() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    void File(std::string_view name, std::string_view bytes) const {
        const auto path = root / paths::PathFromUtf8(name);
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write mod fixture");
        }
    }
    void Manifest(const json& document) const {
        File("mod.json", document.dump());
    }
};

json Manifest(std::string_view id = "test.base", std::string_view tier = "sim_data") {
    return {
        {"schema", "luminumbra.mod.manifest.v1"}, {"id", id}, {"version", "1.2.0"}, {"tier", tier}};
}

struct Result {
    int status;
    std::string bytes;
    json document;
};

Result Invoke(const std::vector<fs::path>& arguments) {
    std::ostringstream output;
    const auto status = Luminumbra::Lumpack::Run(arguments, output);
    return {status, output.str(), json::parse(output.str())};
}

void ExpectRefusal(const Result& result, std::string_view code) {
    EXPECT_EQ(result.status, 1);
    EXPECT_EQ(result.document.at("valid"), false);
    EXPECT_EQ(result.document.at("load_order"), json::array());
    ASSERT_EQ(result.document.at("diagnostics").size(), 1u);
    EXPECT_EQ(result.document.at("diagnostics").at(0).at("code"), code);
}

TEST(LumpackTest, ValidFixtureTiersReuseManifestParserAndKeepFilesUnchanged) {
    for (const auto* tier :
         {"client_cosmetic", "sim_data", "sim_script", "sim_native", "client_native"}) {
        SCOPED_TRACE(tier);
        const Tree tree;
        auto document = Manifest("test.base", tier);
        document["future_metadata"] = {{"allowed", true}};
        tree.Manifest(document);
        tree.File("render/ui/theme.rml", "theme");
        tree.File("client/textures/leaf", "texture");
        const auto result = Invoke({"validate", tree.root});
        EXPECT_EQ(result.status, 0);
        EXPECT_EQ(
            result.document,
            (json{{"diagnostics", json::array()}, {"load_order", {"test.base"}}, {"valid", true}}));
        std::ifstream input(tree.root / "mod.json", std::ios::binary);
        std::ostringstream stored;
        stored << input.rdbuf();
        EXPECT_EQ(stored.str(), document.dump());
        EXPECT_EQ(std::distance(fs::directory_iterator(tree.root), fs::directory_iterator()), 3);
    }
}

TEST(LumpackTest, UsageHasStableJsonAndExitCodeBeforeFilesystemAccess) {
    std::vector<std::vector<fs::path>> cases{{}, {"validate"}, {"pack", "missing"}};
    cases.emplace_back(luminumbra::mods::kMaxModCount + 2, "missing");
    cases.back().front() = "validate";
    std::string expected;
    for (const auto& arguments : cases) {
        const auto result = Invoke(arguments);
        EXPECT_EQ(result.status, 2);
        EXPECT_EQ(result.document.at("valid"), false);
        EXPECT_EQ(result.document.at("diagnostics").at(0).at("code"), "usage");
        if (expected.empty()) {
            expected = result.bytes;
        }
        EXPECT_EQ(result.bytes, expected);
    }
}

TEST(LumpackTest, MissingAndWrongTypeManifestsAndRootsRefuse) {
    const Tree tree;
    ExpectRefusal(Invoke({"validate", tree.root}), "manifest_missing");
    ExpectRefusal(Invoke({"validate", tree.root / "absent"}), "content");
    ExpectRefusal(Invoke({"validate", fs::path{}}), "content");
    ExpectRefusal(Invoke({"validate", fs::path(std::string(32768, 'x'))}), "content");
    tree.File("file", "data");
    ExpectRefusal(Invoke({"validate", tree.root / "file"}), "content");
    fs::create_directory(tree.root / "mod.json");
    ExpectRefusal(Invoke({"validate", tree.root}), "manifest_read");
}

TEST(LumpackTest, InvalidFixtureManifestsRefuseThroughEngineParser) {
    std::vector<std::string> documents{"", "{", "{}", "[]", "null"};
    auto document = Manifest();
    document["schema"] = "unknown";
    documents.push_back(document.dump());
    document = Manifest();
    document["version"] = "1.2";
    documents.push_back(document.dump());
    document = Manifest();
    document["tier"] = "unknown";
    documents.push_back(document.dump());
    document = Manifest();
    document["id"] = "../escape";
    documents.push_back(document.dump());
    documents.push_back(Manifest().dump().insert(1, "\"id\":\"test.other\","));
    for (const auto& bytes : documents) {
        const Tree tree;
        tree.File("mod.json", bytes);
        ExpectRefusal(Invoke({"validate", tree.root}), "manifest");
    }
}

TEST(LumpackTest, ManifestByteLimitAcceptsExactBoundAndRefusesExcessBeforeParsing) {
    const Tree tree;
    auto bytes = Manifest().dump();
    bytes.resize(luminumbra::mods::kMaxManifestBytes, ' ');
    tree.File("mod.json", bytes);
    EXPECT_EQ(Invoke({"validate", tree.root}).status, 0);
    bytes.push_back(' ');
    tree.File("mod.json", bytes);
    ExpectRefusal(Invoke({"validate", tree.root}), "manifest_size");
}

TEST(LumpackTest, RequiredDependenciesResolveAcrossArgumentsInDeterministicOrder) {
    const Tree base, child;
    base.Manifest(Manifest("test.zbase"));
    auto document = Manifest("test.achild", "sim_script");
    document["dependencies"] = {{{"id", "test.zbase"}, {"version", "^1.0.0"}}};
    child.Manifest(document);
    ExpectRefusal(Invoke({"validate", child.root}), "dependencies");
    const auto forward = Invoke({"validate", child.root, base.root});
    EXPECT_EQ(forward.status, 0);
    EXPECT_EQ(forward.document.at("load_order"), (json{"test.zbase", "test.achild"}));
    EXPECT_EQ(forward.bytes, Invoke({"validate", base.root, child.root}).bytes);
    document["dependencies"][0]["version"] = ">=2.0.0";
    child.Manifest(document);
    ExpectRefusal(Invoke({"validate", child.root, base.root}), "dependencies");
}

TEST(LumpackTest, OptionalDependenciesAreOptionalButPresentVersionsMustMatch) {
    const Tree base, child;
    base.Manifest(Manifest("test.base"));
    auto document = Manifest("test.child");
    document["dependencies"] = {{{"id", "test.base"}, {"version", "^1.0.0"}, {"optional", true}}};
    child.Manifest(document);
    EXPECT_EQ(Invoke({"validate", child.root}).status, 0);
    EXPECT_EQ(Invoke({"validate", child.root, base.root}).status, 0);
    document["dependencies"][0]["version"] = "^2.0.0";
    child.Manifest(document);
    ExpectRefusal(Invoke({"validate", child.root, base.root}), "dependencies");
}

TEST(LumpackTest, DuplicateConflictAndCycleRefusalsAreIndependentOfArgumentOrder) {
    const Tree first, second;
    first.Manifest(Manifest("test.first"));
    second.Manifest(Manifest("test.first"));
    ExpectRefusal(Invoke({"validate", first.root, second.root}), "dependencies");
    second.Manifest(Manifest("test.second"));
    auto document = Manifest("test.first");
    document["incompatible_with"] = {{{"id", "test.second"}, {"version", "*"}}};
    first.Manifest(document);
    const auto conflict = Invoke({"validate", first.root, second.root});
    ExpectRefusal(conflict, "dependencies");
    EXPECT_EQ(conflict.bytes, Invoke({"validate", second.root, first.root}).bytes);
    document = Manifest("test.first");
    document["load_order"] = {{"after", {"test.second"}}};
    first.Manifest(document);
    document = Manifest("test.second");
    document["load_order"] = {{"after", {"test.first"}}};
    second.Manifest(document);
    const auto cycle = Invoke({"validate", first.root, second.root});
    ExpectRefusal(cycle, "dependencies");
    EXPECT_EQ(cycle.bytes, Invoke({"validate", second.root, first.root}).bytes);
    EXPECT_NE(cycle.document.at("diagnostics")
                  .at(0)
                  .at("message")
                  .get<std::string>()
                  .find("test.first -> test.second -> test.first"),
              std::string::npos);
}

TEST(LumpackTest, DiagnosticsAreSortedAndInvalidPackagesCannotHideBehindOverlays) {
    const Tree tree;
    tree.File("z/mod.json", "{");
    tree.File("a/mod.json", "{}");
    const auto result = Invoke({"validate", tree.root / "z", tree.root / "a"});
    EXPECT_EQ(result.status, 1);
    const auto& diagnostics = result.document.at("diagnostics");
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics.at(0).at("path"), paths::GenericUtf8PathString(tree.root / "a"));
    EXPECT_EQ(diagnostics.at(1).at("path"), paths::GenericUtf8PathString(tree.root / "z"));
    EXPECT_EQ(result.bytes, Invoke({"validate", tree.root / "a", tree.root / "z"}).bytes);
    tree.File("a/mod.json", Manifest().dump());
    ExpectRefusal(Invoke({"validate", tree.root / "z", tree.root / "a"}), "manifest");
}

TEST(LumpackTest, CosmeticSimulationEntriesRefuseIncludingEmptyAndCaseAliases) {
    for (const auto* name : {"sim", "SIM", "SiM"}) {
        const Tree tree;
        tree.Manifest(Manifest("test.cosmetic", "client_cosmetic"));
        fs::create_directory(tree.root / name);
        ExpectRefusal(Invoke({"validate", tree.root}), "content");
        tree.File(std::string(name) + "/rules", "rules");
        ExpectRefusal(Invoke({"validate", tree.root}), "content");
        tree.Manifest(Manifest("test.rules", "sim_data"));
        EXPECT_EQ(Invoke({"validate", tree.root}).status, 0);
    }
}

TEST(LumpackTest, Utf8PhysicalRootsProduceEscapedMachineReadableJson) {
    const Tree tree;
    tree.File("caf\xc3\xa9/mod.json", "{");
    const auto root = tree.root / paths::PathFromUtf8("caf\xc3\xa9");
    const auto result = Invoke({"validate", root});
    ExpectRefusal(result, "manifest");
    EXPECT_EQ(result.document.at("diagnostics").at(0).at("path"),
              paths::GenericUtf8PathString(root));
    EXPECT_NE(result.bytes.find("caf\\u00e9"), std::string::npos);
    tree.File("caf\xc3\xa9/mod.json", Manifest().dump());
    EXPECT_EQ(Invoke({"validate", root}).status, 0);
}

class RefusingBuffer final : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override {
        return 0;
    }
};

class FlushFailureBuffer final : public std::stringbuf {
    int sync() override {
        return -1;
    }
};

TEST(LumpackTest, FailedWritesAndFlushesNeverReturnSuccess) {
    const Tree tree;
    tree.Manifest(Manifest());
    const std::vector<fs::path> arguments{"validate", tree.root};
    RefusingBuffer buffer;
    std::ostream output(&buffer);
    EXPECT_EQ(Luminumbra::Lumpack::Run(arguments, output), 1);
    FlushFailureBuffer flush_buffer;
    std::ostream flush_output(&flush_buffer);
    EXPECT_EQ(Luminumbra::Lumpack::Run(arguments, flush_output), 1);
    std::ostream throwing_output(&buffer);
    throwing_output.exceptions(std::ios::failbit | std::ios::badbit);
    EXPECT_EQ(Luminumbra::Lumpack::Run(arguments, throwing_output), 1);
}

#if defined(_WIN32) && !defined(__CYGWIN__)
// Directory junction creation works without symbolic-link creation privilege.
void CreateJunction(const fs::path& link, const fs::path& target) {
    ASSERT_TRUE(fs::create_directory(link));
    const HANDLE handle = CreateFileW(link.c_str(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    struct JunctionData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        std::array<wchar_t, 2048> names; // Bounded junction target, including terminators.
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + target.native();
    if (substitute.size() + 2 > data.names.size()) {
        CloseHandle(handle);
        FAIL() << "junction target exceeds fixture bound";
    }
    std::copy(substitute.begin(), substitute.end(), data.names.begin());
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.data_length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    DWORD returned = 0;
    const BOOL result = DeviceIoControl(handle,
                                        FSCTL_SET_REPARSE_POINT,
                                        &data,
                                        static_cast<DWORD>(8 + data.data_length),
                                        nullptr,
                                        0,
                                        &returned,
                                        nullptr);
    const DWORD failure = GetLastError();
    CloseHandle(handle);
    ASSERT_NE(result, 0) << failure;
}

TEST(LumpackTest, WindowsJunctionsRefuseWithoutSymlinkPrivilege) {
    const Tree tree, outside;
    tree.Manifest(Manifest());
    outside.Manifest(Manifest());
    ASSERT_NO_FATAL_FAILURE(CreateJunction(tree.root / "redirect", outside.root));
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
    ExpectRefusal(Invoke({"validate", tree.root / "redirect"}), "content");
}

TEST(LumpackTest, WindowsAbsoluteAndShortRootSpellingsUseNativePaths) {
    const Tree tree;
    tree.Manifest(Manifest());
    EXPECT_TRUE(paths::IsAbsolutePath(tree.root));
    EXPECT_EQ(Invoke({"validate", tree.root}).status, 0);
    std::array<wchar_t, 32768> spelling{}; // Extended Win32 path including terminator.
    const DWORD length =
        GetShortPathNameW(tree.root.c_str(), spelling.data(), static_cast<DWORD>(spelling.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, spelling.size());
    EXPECT_EQ(Invoke({"validate", fs::path(std::wstring(spelling.data(), length))}).status, 0);
}

TEST(LumpackTest, WindowsUnreadableManifestRefusesWhileOwningHandleRemainsOpen) {
    const Tree tree;
    tree.Manifest(Manifest());
    const HANDLE handle = CreateFileW((tree.root / "mod.json").c_str(),
                                      GENERIC_READ,
                                      0,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
    EXPECT_NE(CloseHandle(handle), 0);
}
#else
TEST(LumpackTest, SymlinkFilesDirectoriesRootsAndDanglingLinksRefuse) {
    const Tree tree, outside;
    tree.Manifest(Manifest());
    outside.Manifest(Manifest());
    fs::create_symlink(outside.root / "mod.json", tree.root / "link");
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
    ASSERT_TRUE(fs::remove(tree.root / "link"));
    fs::create_directory_symlink(outside.root, tree.root / "redirect");
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
    ExpectRefusal(Invoke({"validate", tree.root / "redirect"}), "content");
    ASSERT_TRUE(fs::remove(tree.root / "redirect"));
    fs::create_symlink(outside.root / "absent", tree.root / "dangling");
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
    ASSERT_TRUE(fs::remove(tree.root / "dangling"));
    ASSERT_TRUE(fs::remove(tree.root / "mod.json"));
    fs::create_symlink(outside.root / "mod.json", tree.root / "mod.json");
    ExpectRefusal(Invoke({"validate", tree.root}), "content");
}

TEST(LumpackTest, SpecialFilesAndInvalidLogicalNamesRefuseWithoutBlocking) {
    const Tree fifo;
    fifo.Manifest(Manifest());
    ASSERT_EQ(mkfifo((fifo.root / "fifo").c_str(), 0600), 0);
    ExpectRefusal(Invoke({"validate", fifo.root}), "content");
    for (const auto* name : {"bad\\name", "bad:name", "bad.", "bad ", "bad\x80"}) {
        const Tree tree;
        tree.Manifest(Manifest());
        tree.File(name, "data");
        ExpectRefusal(Invoke({"validate", tree.root}), "content");
    }
}

TEST(LumpackTest, UnreadableManifestRefusesWhenOperatingSystemEnforcesPermissions) {
    const Tree tree;
    tree.Manifest(Manifest());
    const auto manifest = tree.root / "mod.json";
    fs::permissions(manifest, fs::perms::none);
    // Privileged processes may still read; keep the type/symlink refusals unconditional.
    std::ifstream probe(manifest);
    if (!probe) {
        ExpectRefusal(Invoke({"validate", tree.root}), "content");
    }
    probe.close();
    fs::permissions(manifest, fs::perms::owner_all);
    EXPECT_EQ(Invoke({"validate", tree.root}).status, 0);
}
#endif
} // namespace
