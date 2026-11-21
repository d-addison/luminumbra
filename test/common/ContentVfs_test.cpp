#include "../support/UniqueTempPath.h"
#include "luminumbra_common/vfs/ContentVfs.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
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
using Luminumbra::Vfs::ContentResidency;
using Luminumbra::Vfs::ContentVfs;

struct Tree {
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("vfs-");
    ~Tree() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path File(std::string_view name, std::string_view bytes = "content") const {
        const auto path = root / paths::PathFromUtf8(name);
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write content fixture");
        }
        return path;
    }
};

std::string Read(const ContentVfs& vfs, std::string_view name) {
    auto input = vfs.Open(name);
    if (!input) {
        throw std::runtime_error("expected content fixture is absent");
    }
    std::array<char, 65536> buffer{}; // One fixed-size verified read buffer.
    const auto count = input->read(buffer);
    EXPECT_EQ(count, input->size());
    EXPECT_EQ(input->read(buffer), 0u);
    return {buffer.data(), count};
}

std::vector<std::string> Names(const std::vector<ContentVfs::Entry>& entries) {
    std::vector<std::string> names;
    for (const auto& entry : entries) {
        names.push_back(entry.name);
    }
    return names;
}

// NTFS directory junctions do not require symbolic-link creation privilege.
#if defined(_WIN32) && !defined(__CYGWIN__)
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
    // A mount-point payload with a bounded substitute name and empty print name.
    struct JunctionData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        std::array<wchar_t, 2048> names;
    } data{};
    const auto substitute = std::wstring(L"\\??\\") + fs::absolute(target).native();
    if (substitute.size() + 2 > data.names.size()) {
        CloseHandle(handle);
        FAIL() << "junction fixture exceeds name buffer";
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
#endif
} // namespace

TEST(ContentVfsTest, ThreeMountsMergeInUtf8ByteOrderAndLastProviderWins) {
    const Tree base, first, last;
    base.File("client/z", "base-z");
    base.File("client/shared", "base");
    base.File("sim/rules", "rules");
    first.File("client/\xc3\xa9", "accent");
    first.File("client/shared", "first");
    last.File("client/a", "a");
    last.File("client/\xf0\x9f\x8c\xb2", "tree");
    last.File("client/shared", "last");
    ContentVfs vfs;
    vfs.AddMount(base.root, ContentResidency::SimData);
    vfs.AddMount(first.root, ContentResidency::ClientCosmetic);
    vfs.AddMount(last.root, ContentResidency::ClientCosmetic);
    EXPECT_EQ(vfs.MountCount(), 3u);
    EXPECT_EQ(Names(vfs.ListDirectory()), (std::vector<std::string>{"client", "sim"}));
    const auto entries = vfs.ListDirectory("client");
    EXPECT_EQ(Names(entries),
              (std::vector<std::string>{"a", "shared", "z", "\xc3\xa9", "\xf0\x9f\x8c\xb2"}));
    ASSERT_EQ(entries.size(), 5u);
    EXPECT_EQ(entries[1].residency, ContentResidency::ClientCosmetic);
    EXPECT_EQ(entries[1].mount_index, 2u);
    EXPECT_FALSE(entries[1].directory);
    EXPECT_EQ(Read(vfs, "client/shared"), "last");
    EXPECT_EQ(Read(vfs, "client/z"), "base-z");
    EXPECT_EQ(Read(vfs, "client/\xc3\xa9"), "accent");
    EXPECT_EQ(Read(vfs, "client/\xf0\x9f\x8c\xb2"), "tree");
    EXPECT_EQ(Read(vfs, "sim/rules"), "rules");

    ContentVfs reversed;
    reversed.AddMount(base.root, ContentResidency::SimData);
    reversed.AddMount(last.root, ContentResidency::ClientCosmetic);
    reversed.AddMount(first.root, ContentResidency::ClientCosmetic);
    EXPECT_EQ(Names(reversed.ListDirectory("client")), Names(entries));
    EXPECT_EQ(Read(reversed, "client/shared"), "first");
}

TEST(ContentVfsTest, InventoryOrderDoesNotDependOnCreationOrder) {
    const Tree forward, reverse;
    for (const auto* name : {"c", "a", "b"}) {
        forward.File(name, name);
    }
    for (const auto* name : {"b", "a", "c"}) {
        reverse.File(name, name);
    }
    ContentVfs lhs, rhs;
    lhs.AddMount(forward.root, ContentResidency::Native);
    rhs.AddMount(reverse.root, ContentResidency::Native);
    EXPECT_EQ(lhs.ListDirectory(), rhs.ListDirectory());
}

TEST(ContentVfsTest, EmptyAndMissingAreDistinctFromWrongTypeAndRemovedProviders) {
    const Tree tree;
    ContentVfs vfs;
    EXPECT_EQ(vfs.Open("absent"), nullptr);
    EXPECT_TRUE(vfs.ListDirectory().empty());
    EXPECT_TRUE(vfs.ListDirectory("absent").empty());
    vfs.AddMount(tree.root / "", ContentResidency::ClientCosmetic);
    EXPECT_EQ(vfs.MountCount(), 1u);
    EXPECT_TRUE(vfs.ListDirectory().empty());
    tree.File("client/empty", "");
    tree.File("client/a");
    vfs.AddMount(tree.root, ContentResidency::ClientCosmetic);
    EXPECT_EQ(Read(vfs, "client/empty"), "");
    EXPECT_EQ(vfs.Open("client/A"), nullptr);
    EXPECT_THROW(static_cast<void>(vfs.Open("client")), std::runtime_error);
    EXPECT_THROW(static_cast<void>(vfs.ListDirectory("client/a")), std::runtime_error);
    ASSERT_TRUE(fs::remove(tree.root / "client/a"));
    EXPECT_THROW(static_cast<void>(vfs.Open("client/a")), std::runtime_error);
    ASSERT_TRUE(fs::create_directory(tree.root / "client/a"));
    EXPECT_THROW(static_cast<void>(vfs.Open("client/a")), std::runtime_error);
}

TEST(ContentVfsTest, FilesHideSubtreesAndDirectoriesCanReplaceFiles) {
    const Tree base, middle, last;
    base.File("node/old");
    middle.File("node", "replacement");
    last.File("node/new", "new");
    ContentVfs vfs;
    vfs.AddMount(base.root, ContentResidency::SimData);
    vfs.AddMount(middle.root, ContentResidency::Native);
    EXPECT_EQ(vfs.Open("node/old"), nullptr);
    EXPECT_EQ(Read(vfs, "node"), "replacement");
    EXPECT_THROW(static_cast<void>(vfs.ListDirectory("node")), std::runtime_error);
    vfs.AddMount(last.root, ContentResidency::SimScript);
    EXPECT_THROW(static_cast<void>(vfs.Open("node")), std::runtime_error);
    EXPECT_EQ(Names(vfs.ListDirectory("node")), (std::vector<std::string>{"new"}));
    EXPECT_EQ(Read(vfs, "node/new"), "new");
}

TEST(ContentVfsTest, CosmeticSimulationFilesAndEmptyDirectoriesRefuseAtomically) {
    const Tree base;
    base.File("client/a", "base");
    ContentVfs vfs;
    vfs.AddMount(base.root, ContentResidency::SimData);
    const auto before = vfs.ListDirectory();
    for (const auto* name : {"sim/rules", "sim/empty", "SIM/rules", "SiM/empty", "sim"}) {
        const Tree cosmetic;
        cosmetic.File("client/a", "override");
        if (std::string_view(name).ends_with("empty")) {
            fs::create_directories(cosmetic.root / name);
        } else {
            cosmetic.File(name);
        }
        EXPECT_THROW(vfs.AddMount(cosmetic.root, ContentResidency::ClientCosmetic),
                     std::runtime_error);
        EXPECT_EQ(vfs.MountCount(), 1u);
        EXPECT_EQ(vfs.ListDirectory(), before);
        EXPECT_EQ(Read(vfs, "client/a"), "base");
    }
}

TEST(ContentVfsTest, SimulationAndNativeResidenciesArePreserved) {
    const Tree tree;
    tree.File("sim/rules");
    for (const auto residency :
         {ContentResidency::SimData, ContentResidency::SimScript, ContentResidency::Native}) {
        ContentVfs vfs;
        vfs.AddMount(tree.root, residency);
        const auto entries = vfs.ListDirectory("sim");
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries.front().residency, residency);
        EXPECT_EQ(Read(vfs, "sim/rules"), "content");
    }
}

TEST(ContentVfsTest, LexicalRefusalsApplyOnEveryPlatformBeforeLookup) {
    const Tree tree;
    tree.File("client/file");
    ContentVfs vfs;
    vfs.AddMount(tree.root, ContentResidency::ClientCosmetic);
    const std::vector<std::string> invalid{"",
                                           ".",
                                           "..",
                                           "/client/file",
                                           "//host/share/file",
                                           "C:",
                                           "C:client/file",
                                           "C:/client/file",
                                           "client//file",
                                           "client/",
                                           "./client/file",
                                           "client/./file",
                                           "client/../file",
                                           "../file",
                                           "client\\file",
                                           "\\host\\file",
                                           "client/file:stream",
                                           "client/file.",
                                           "client/file ",
                                           std::string("client/\0file", 12),
                                           "client/\x80",
                                           "client/\xc0\xaf",
                                           "client/\xed\xa0\x80",
                                           "client/\xf4\x90\x80\x80",
                                           "client/\xe2\x82",
                                           std::string(4097, 'x')};
    for (const auto& path : invalid) {
        SCOPED_TRACE(path);
        EXPECT_THROW(static_cast<void>(vfs.Open(path)), std::invalid_argument);
        EXPECT_THROW(static_cast<void>(vfs.ListDirectory(path)), std::invalid_argument);
    }
    EXPECT_EQ(Read(vfs, "client/file"), "content");
}

TEST(ContentVfsTest, InvalidRootsResidenciesAndLimitsRefuseWithoutPublication) {
    const Tree tree;
    tree.File("file");
    ContentVfs vfs;
    EXPECT_THROW(vfs.AddMount({}, ContentResidency::SimData), std::invalid_argument);
    EXPECT_THROW(vfs.AddMount(tree.root, static_cast<ContentResidency>(255)),
                 std::invalid_argument);
    EXPECT_THROW(vfs.AddMount(tree.root / "missing", ContentResidency::SimData),
                 std::runtime_error);
    EXPECT_THROW(vfs.AddMount(tree.root / "file", ContentResidency::SimData), std::runtime_error);
    EXPECT_EQ(vfs.MountCount(), 0u);
    for (const ContentVfs::Limits& limits : {ContentVfs::Limits{0, 1, 1, 1},
                                             ContentVfs::Limits{1, 0, 1, 1},
                                             ContentVfs::Limits{1, 1, 0, 1},
                                             ContentVfs::Limits{1, 1, 32768, 1},
                                             ContentVfs::Limits{1, 1, 1, 0}}) {
        EXPECT_THROW((ContentVfs{limits}), std::invalid_argument);
    }
}

TEST(ContentVfsTest, InventoryBudgetsAreCheckedForScansAndMergedMounts) {
    const Tree base, next, crowded, long_name;
    base.File("a");
    next.File("b");
    crowded.File("b");
    crowded.File("c");
    long_name.File("long");
    for (const ContentVfs::Limits& limits : {ContentVfs::Limits{1, 4, 4, 16},
                                             ContentVfs::Limits{4, 1, 4, 16},
                                             ContentVfs::Limits{4, 4, 4, 1}}) {
        ContentVfs vfs(limits);
        vfs.AddMount(base.root, ContentResidency::SimData);
        EXPECT_THROW(vfs.AddMount(next.root, ContentResidency::SimScript), std::exception);
        EXPECT_EQ(vfs.MountCount(), 1u);
        EXPECT_EQ(Read(vfs, "a"), "content");
    }
    for (const ContentVfs::Limits& limits : {ContentVfs::Limits{4, 1, 4, 16},
                                             ContentVfs::Limits{4, 4, 4, 1},
                                             ContentVfs::Limits{4, 4, 1, 16}}) {
        ContentVfs vfs(limits);
        EXPECT_THROW(vfs.AddMount(limits.max_path_bytes == 1 ? long_name.root : crowded.root,
                                  ContentResidency::SimData),
                     std::exception);
        EXPECT_EQ(vfs.MountCount(), 0u);
    }
    ContentVfs exact(ContentVfs::Limits{2, 1, 1, 1});
    exact.AddMount(base.root, ContentResidency::SimData);
    exact.AddMount(base.root, ContentResidency::SimScript);
    EXPECT_EQ(exact.MountCount(), 2u);
    EXPECT_EQ(Read(exact, "a"), "content");
}

TEST(ContentVfsTest, LeafAndIntermediateLinksAreRefusedAtMountAndOpen) {
    const Tree base, outside;
    base.File("client/file", "base");
    outside.File("file", "outside");
    ContentVfs vfs;
    vfs.AddMount(base.root, ContentResidency::ClientCosmetic);
    std::error_code error;
    const auto leaf = base.root / "client/link";
    fs::create_symlink(outside.root / "file", leaf, error);
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (error) {
        GTEST_SKIP() << "symbolic link creation unavailable: " << error.message();
    }
#else
    ASSERT_FALSE(error) << error.message();
#endif
    EXPECT_THROW(vfs.AddMount(base.root, ContentResidency::ClientCosmetic), std::runtime_error);
    ASSERT_TRUE(fs::remove(leaf));
    const auto original = base.root / "client/file";
    ASSERT_TRUE(fs::remove(original));
    fs::create_symlink(outside.root / "file", original);
    EXPECT_THROW(static_cast<void>(vfs.Open("client/file")), std::runtime_error);
    ASSERT_TRUE(fs::remove(original));
    base.File("client/file");
    fs::rename(base.root / "client", base.root / "saved");
    // A redirect to an in-root directory must also refuse, regardless of containment.
    fs::create_directory_symlink(base.root / "saved", base.root / "client");
    EXPECT_THROW(static_cast<void>(vfs.Open("client/file")), std::runtime_error);
    EXPECT_THROW(vfs.AddMount(base.root, ContentResidency::ClientCosmetic), std::runtime_error);
    ASSERT_TRUE(fs::remove(base.root / "client"));
    fs::create_directory_symlink(outside.root, base.root / "client");
    EXPECT_THROW(static_cast<void>(vfs.Open("client/file")), std::runtime_error);
    EXPECT_THROW(vfs.AddMount(base.root / "client", ContentResidency::SimData), std::runtime_error);
    EXPECT_EQ(vfs.MountCount(), 1u);
    ASSERT_TRUE(fs::remove(base.root / "client"));
    fs::create_directory_symlink(base.root / "saved", base.root / "client");
    fs::create_directory(base.root / "saved/nested");
    EXPECT_THROW(vfs.AddMount(base.root / "client/nested", ContentResidency::SimData),
                 std::runtime_error);
    ASSERT_TRUE(fs::remove(base.root / "client"));
    fs::create_symlink(outside.root / "absent", base.root / "dangling");
    EXPECT_THROW(vfs.AddMount(base.root, ContentResidency::SimData), std::runtime_error);
}

#if defined(_WIN32) && !defined(__CYGWIN__)
TEST(ContentVfsTest, WindowsShortMountRootsKeepExactLogicalNames) {
    const Tree tree;
    tree.File("client/long-asset-name.txt", "asset");
    std::array<wchar_t, 32768> spelling{}; // Extended Win32 path including terminator.
    const DWORD length =
        GetShortPathNameW(tree.root.c_str(), spelling.data(), static_cast<DWORD>(spelling.size()));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, spelling.size());
    // Volumes with 8.3 generation disabled return the original long spelling.
    ContentVfs vfs;
    vfs.AddMount(fs::path(std::wstring(spelling.data(), length)), ContentResidency::ClientCosmetic);
    EXPECT_EQ(Read(vfs, "client/long-asset-name.txt"), "asset");
    EXPECT_EQ(vfs.Open("CLIENT/long-asset-name.txt"), nullptr);
}

TEST(ContentVfsTest, IntermediateJunctionRefusesWithoutSymlinkPrivilege) {
    const Tree base, outside;
    base.File("client/file");
    outside.File("file");
    ContentVfs vfs;
    vfs.AddMount(base.root, ContentResidency::ClientCosmetic);
    fs::rename(base.root / "client", base.root / "saved");
    ASSERT_NO_FATAL_FAILURE(CreateJunction(base.root / "client", outside.root));
    EXPECT_THROW(static_cast<void>(vfs.Open("client/file")), std::runtime_error);
    EXPECT_THROW(vfs.AddMount(base.root, ContentResidency::ClientCosmetic), std::runtime_error);
    EXPECT_EQ(vfs.MountCount(), 1u);
    ASSERT_TRUE(fs::remove(base.root / "client"));
}
#else
TEST(ContentVfsTest, InvalidOnDiskLogicalNamesRefuseAtMount) {
    for (const auto* name : {"bad\\name", "bad:name", "bad.", "bad ", "bad\x80"}) {
        const Tree tree;
        const auto filename = tree.root / fs::path(name);
        std::ofstream output(filename);
        output << "fixture";
        output.close();
        ASSERT_TRUE(output);
        ContentVfs vfs;
        EXPECT_THROW(vfs.AddMount(tree.root, ContentResidency::SimData), std::invalid_argument);
        EXPECT_EQ(vfs.MountCount(), 0u);
    }
}

TEST(ContentVfsTest, FifoAndUnreadableFilesRefuseWithoutBlockingOrPublishing) {
    const Tree tree;
    ASSERT_EQ(mkfifo((tree.root / "fifo").c_str(), 0600), 0);
    ContentVfs vfs;
    EXPECT_THROW(vfs.AddMount(tree.root, ContentResidency::SimData), std::runtime_error);
    ASSERT_TRUE(fs::remove(tree.root / "fifo"));
    const auto file = tree.File("file");
    vfs.AddMount(tree.root, ContentResidency::SimData);
    fs::permissions(file, fs::perms::none);
    // Privileged runners can still read a file with no permission bits. The type refusal above
    // remains unconditional; exercise access refusal whenever the OS enforces it.
    std::ifstream probe(file);
    if (!probe) {
        EXPECT_THROW(static_cast<void>(vfs.Open("file")), std::runtime_error);
        EXPECT_THROW(vfs.AddMount(tree.root, ContentResidency::SimData), std::runtime_error);
    }
    probe.close();
    fs::permissions(file, fs::perms::owner_all);
    EXPECT_EQ(vfs.MountCount(), 1u);
}
#endif
