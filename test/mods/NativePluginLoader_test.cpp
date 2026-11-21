#include "authoring/PrefabDigest.h"
#include "mods/NativePluginLoader.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "core/FilesystemPath.h"
#include "core/NativeFileOwner.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
using Luminumbra::Mods::NativePluginLoader;
using Result = Luminumbra::Mods::NativePluginResult;

static_assert(std::is_standard_layout_v<LumHostApi> && std::is_trivial_v<LumHostApi>);
static_assert(std::is_standard_layout_v<LumPluginInfo> && std::is_trivial_v<LumPluginInfo>);
static_assert(std::is_standard_layout_v<LumSlice> && std::is_trivial_v<LumSlice>);
static_assert(std::is_standard_layout_v<LumStrView> && std::is_trivial_v<LumStrView>);

struct Events {
    int initialized = 0;
    int stopped = 0;
};

Events& Recorded() {
    static Events events;
    return events;
}

extern "C" void LUM_CALL Initialized() {
    ++Recorded().initialized;
}

extern "C" void LUM_CALL Stopped() {
    ++Recorded().stopped;
}

extern "C" LumProc LUM_CALL Resolve(const char* name) {
    if (std::strcmp(name, "test_initialized") == 0) {
        return Initialized;
    }
    if (std::strcmp(name, "test_stopped") == 0) {
        return Stopped;
    }
    return nullptr;
}

LumHostApi Host() {
    return {sizeof(LumHostApi), LUM_ABI_MAJOR, LUM_ABI_MINOR, Resolve};
}

std::filesystem::path Plugin(const std::string& variant = "good") {
#if defined(_WIN32)
    constexpr auto suffix = ".dll";
#else
    constexpr auto suffix = ".so";
#endif
    return std::filesystem::current_path() / ("native_plugin_" + variant + suffix);
}

std::string Pin(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                          std::istreambuf_iterator<char>()};
    return Luminumbra::Authoring::Detail::Sha256(bytes);
}

class NativePluginLoaderTest : public testing::Test {
protected:
    void SetUp() override {
        Recorded() = {};
        ASSERT_TRUE(std::filesystem::is_regular_file(Plugin()));
    }
};

TEST_F(NativePluginLoaderTest, InitializesAndShutsDownExactlyOnce) {
    NativePluginLoader loader;
    EXPECT_FALSE(loader.active());
    loader.shutdown();
    ASSERT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::Loaded);
    EXPECT_TRUE(loader.active());
    EXPECT_EQ(Recorded().initialized, 1);
    EXPECT_EQ(Recorded().stopped, 0);
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::AlreadyLoaded);
    loader.shutdown();
    loader.shutdown();
    EXPECT_FALSE(loader.active());
    EXPECT_EQ(Recorded().stopped, 1);
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::AlreadyLoaded);
}

TEST_F(NativePluginLoaderTest, DestructorShutsDownIncludingNullContext) {
    for (const auto* variant : {"good", "null_context"}) {
        Recorded() = {};
        {
            NativePluginLoader loader;
            const auto path = Plugin(variant);
            ASSERT_EQ(loader.load(path, Pin(path), true, Host()), Result::Loaded);
        }
        EXPECT_EQ(Recorded().initialized, 1);
        EXPECT_EQ(Recorded().stopped, 1);
    }
}

TEST_F(NativePluginLoaderTest, TrustPrecedesAllFileAndHostAccess) {
    NativePluginLoader loader;
    EXPECT_EQ(loader.load({}, {}, false, {}), Result::Untrusted);
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), false, Host()), Result::Untrusted);
    EXPECT_FALSE(loader.active());
    EXPECT_EQ(Recorded().initialized, 0);
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::Loaded);
}

TEST_F(NativePluginLoaderTest, RefusesInvalidHostAndPinsBeforeLoading) {
    const auto valid = Host();
    auto bad = valid;
    NativePluginLoader loader;
    for (const auto field : {0, 1, 2, 3}) {
        bad = valid;
        if (field == 0) {
            bad.struct_size = 0;
        } else if (field == 1) {
            ++bad.abi_major;
        } else if (field == 2) {
            ++bad.abi_minor;
        } else {
            bad.get_proc_address = nullptr;
        }
        EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, bad), Result::InvalidHost);
    }
    for (const auto& pin : {std::string{},
                            std::string(63, '0'),
                            std::string(65, '0'),
                            std::string(64, 'G'),
                            std::string(64, 'A')}) {
        EXPECT_EQ(loader.load(Plugin(), pin, true, valid), Result::InvalidPin);
    }
    EXPECT_EQ(loader.load(Plugin(), std::string(64, '0'), true, valid), Result::HashMismatch);
    EXPECT_EQ(Recorded().initialized, 0);
    EXPECT_FALSE(loader.active());
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, valid), Result::Loaded);
}

TEST_F(NativePluginLoaderTest, RefusesAbiQueryAndSymbolFailuresBeforeInit) {
    const std::array cases = {std::pair{"bad_major", Result::IncompatibleAbi},
                              std::pair{"bad_minor", Result::IncompatibleAbi},
                              std::pair{"bad_size", Result::IncompatibleAbi},
                              std::pair{"query_failure", Result::QueryFailed},
                              std::pair{"missing_query", Result::MissingSymbol},
                              std::pair{"missing_init", Result::MissingSymbol},
                              std::pair{"missing_shutdown", Result::MissingSymbol}};
    NativePluginLoader loader;
    for (const auto& [variant, expected] : cases) {
        SCOPED_TRACE(variant);
        const auto path = Plugin(variant);
        EXPECT_EQ(loader.load(path, Pin(path), true, Host()), expected);
        EXPECT_FALSE(loader.active());
        EXPECT_EQ(Recorded().initialized, 0);
        EXPECT_EQ(Recorded().stopped, 0);
    }
    EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::Loaded);
}

TEST_F(NativePluginLoaderTest, FailedInitCleansReturnedContextAndAllowsRetry) {
    for (const auto* variant : {"init_failure", "clean_init_failure"}) {
        Recorded() = {};
        NativePluginLoader loader;
        const auto path = Plugin(variant);
        EXPECT_EQ(loader.load(path, Pin(path), true, Host()), Result::InitFailed);
        EXPECT_FALSE(loader.active());
        EXPECT_EQ(Recorded().initialized, 1);
        const int expected_stops = std::strcmp(variant, "init_failure") == 0 ? 1 : 0;
        EXPECT_EQ(Recorded().stopped, expected_stops);
        loader.shutdown();
        EXPECT_EQ(Recorded().stopped, expected_stops);
        EXPECT_EQ(loader.load(Plugin(), Pin(Plugin()), true, Host()), Result::Loaded);
    }
}

TEST_F(NativePluginLoaderTest, RefusesMissingEmptyDirectoryAndInvalidLibrary) {
    NativePluginLoader loader;
    const auto pin = Pin(Plugin());
    EXPECT_EQ(loader.load({}, pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(loader.load(Plugin("absent"), pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(loader.load(std::filesystem::current_path(), pin, true, Host()), Result::FileRefused);
    const auto embedded_null = std::filesystem::path(std::string("bad\0path", 8));
    EXPECT_EQ(loader.load(embedded_null, pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(loader.load(std::filesystem::path(std::string(32768, 'x')), pin, true, Host()),
              Result::FileRefused);
    const auto file = Plugin("invalid_bytes");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{file};
    {
        std::ofstream output(file, std::ios::binary);
        ASSERT_TRUE(output.good());
    }
    EXPECT_EQ(loader.load(file, pin, true, Host()), Result::FileRefused);
    {
        std::ofstream output(file, std::ios::binary);
        output << "invalid native library";
        output.close();
        ASSERT_FALSE(output.fail());
    }
    EXPECT_EQ(loader.load(file, Pin(file), true, Host()), Result::LoadFailed);
    std::filesystem::resize_file(file, 64U * 1024U * 1024U + 1U);
    EXPECT_EQ(loader.load(file, pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(Recorded().initialized, 0);
}

TEST_F(NativePluginLoaderTest, LoadsRelativeUnicodePathAndRetainsVerifiedImage) {
    const auto directory = Luminumbra::Filesystem::PathFromUtf8("native-plugin-\xc3\xa9");
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto file = directory / Plugin().filename();
    std::filesystem::copy_file(Plugin(), file);
    {
        NativePluginLoader loader;
        ASSERT_EQ(loader.load(file, Pin(file), true, Host()), Result::Loaded);
        std::ofstream overwrite(file, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
        EXPECT_FALSE(overwrite.is_open());
        std::error_code error;
        auto moved = directory;
        moved += "-moved";
        std::filesystem::rename(directory, moved, error);
        EXPECT_TRUE(error);
#else
        ASSERT_TRUE(overwrite.is_open());
        overwrite << "replaced after loading";
        overwrite.close();
        ASSERT_FALSE(overwrite.fail());
#endif
        loader.shutdown();
        EXPECT_EQ(Recorded().initialized, 1);
        EXPECT_EQ(Recorded().stopped, 1);
    }
    std::ofstream overwrite(file, std::ios::binary | std::ios::trunc);
    EXPECT_TRUE(overwrite.is_open());
}

#if defined(_WIN32)
TEST_F(NativePluginLoaderTest, RefusesAnExistingWriterAndDevicePath) {
    NativePluginLoader loader;
    const auto path = Plugin("writer");
    struct Cleanup {
        std::filesystem::path file;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    } cleanup{path};
    std::filesystem::copy_file(Plugin(), path);
    const auto pin = Pin(path);
    Luminumbra::Filesystem::NativeFileOwner writer(CreateFileW(path.c_str(),
                                                               GENERIC_WRITE,
                                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                               nullptr,
                                                               OPEN_EXISTING,
                                                               FILE_ATTRIBUTE_NORMAL,
                                                               nullptr));
    ASSERT_NE(writer.get(), INVALID_HANDLE_VALUE);
    EXPECT_EQ(loader.load(path, pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(loader.load(L"\\\\.\\NUL", pin, true, Host()), Result::FileRefused);
    EXPECT_EQ(Recorded().initialized, 0);
}

TEST_F(NativePluginLoaderTest, AcceptsWindowsHandleAliases) {
    const auto path = Plugin();
    // If 8.3 generation is disabled, GetShortPathNameW returns the long spelling.
    std::wstring spelling(32768, L'\0');
    const auto size =
        GetShortPathNameW(path.c_str(), spelling.data(), static_cast<DWORD>(spelling.size()));
    ASSERT_GT(size, 0U);
    ASSERT_LT(size, spelling.size());
    spelling.resize(size);
    NativePluginLoader loader;
    EXPECT_EQ(loader.load(std::filesystem::path(spelling), Pin(path), true, Host()),
              Result::Loaded);
}
#endif

#if !defined(_WIN32)
TEST_F(NativePluginLoaderTest, RefusesFifoWithoutBlocking) {
    const auto fifo = Plugin("fifo");
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    NativePluginLoader loader;
    EXPECT_EQ(loader.load(fifo, Pin(Plugin()), true, Host()), Result::FileRefused);
    EXPECT_EQ(loader.load("/dev/null", Pin(Plugin()), true, Host()), Result::FileRefused);
    EXPECT_TRUE(std::filesystem::remove(fifo));
}
#endif
} // namespace
