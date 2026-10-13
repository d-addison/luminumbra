#pragma once

// Test-support code only: never include from src/.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace Luminumbra::TestSupport {
namespace detail {

inline constexpr int kTempPathAttempts = 64;

inline std::string CompactHex(std::uint64_t value) {
    std::array<char, 16> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    return {buffer.data(), result.ptr};
}

inline std::string UniqueTempStem(std::string_view prefix) {
    std::string test_name = "notest";
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (test_info != nullptr) {
        test_name = std::string(test_info->test_suite_name()) + "." + test_info->name();
    }
    // FNV-1a hashes every byte of the complete, unsanitised suite.name.
    std::uint32_t name_hash = 2166136261U;
    for (const char character : test_name) {
        name_hash ^= static_cast<unsigned char>(character);
        name_hash *= 16777619U;
    }
    auto hash_text = CompactHex(name_hash);
    hash_text.insert(0, 8 - hash_text.size(), '0');
    if (test_name.size() > 24) {
        test_name.resize(24);
    }
    for (char& character : test_name) {
        if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_')) {
            character = '_';
        }
    }
    // Maximum stem length excluding prefix: 24 + 1 + 8 + 1 + 16 + 1 + 16 = 67.
    // With attempt suffix _63, the review's static_scene example is at most
    // 40 (MSVC temp root) + 1 (separator) + 23 (prefix) + 67 (stem) + 3 (attempt)
    // + 105 (nested mesh path including separator) = 239 chars, 240 with NUL,
    // comfortably below Windows MAX_PATH (260).
    // One counter across all translation units in the test executable. The
    // filesystem reservation, rather than the clock/counter, arbitrates between
    // processes that happen to generate the same directory candidate.
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::string(prefix) + test_name + "_" + hash_text + "_" + CompactHex(stamp) + "_" +
           CompactHex(sequence.fetch_add(1, std::memory_order_relaxed));
}

// Keep candidate reservation separate from name generation so collisions can
// be exercised deterministically, including files occupying directory names.
[[nodiscard]] inline std::filesystem::path ReserveTempDirectory(
    const std::filesystem::path& temporary_root, const std::string& stem, std::error_code& error) {
    for (int attempt = 0; attempt < kTempPathAttempts; ++attempt) {
        const auto candidate = temporary_root / (stem + "_" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (!error) {
            error = std::make_error_code(std::errc::file_exists);
        }
    }
    return {};
}

} // namespace detail

/// Create a fresh directory under the system temporary root, with 64 attempts.
/// On failure, return an empty path and report the last error, including a
/// collision reported by create_directory as false with no error code.
[[nodiscard]] inline std::filesystem::path CreateUniqueTempDirectory(std::string_view prefix,
                                                                     std::error_code& error) {
    const auto temporary_root = std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }
    return detail::ReserveTempDirectory(temporary_root, detail::UniqueTempStem(prefix), error);
}

/// Create an exclusively reserved directory; the caller owns its cleanup.
/// Throw std::runtime_error with the last error if allocation fails.
[[nodiscard]] inline std::filesystem::path CreateUniqueTempDirectory(std::string_view prefix) {
    std::error_code error;
    const auto directory = CreateUniqueTempDirectory(prefix, error);
    if (directory.empty()) {
        throw std::runtime_error("Failed to create a fresh temporary directory: " +
                                 error.message());
    }
    return directory;
}

/// Return a file path inside an exclusively reserved fresh directory, retaining
/// the supplied suffix. The file starts absent; the caller must remove_all the
/// returned path's parent directory, or use ScopedTempFile to own its cleanup.
[[nodiscard]] inline std::filesystem::path UniqueTempFilePath(std::string_view prefix,
                                                              std::string_view suffix) {
    const std::filesystem::path filename("file" + std::string(suffix));
    return CreateUniqueTempDirectory(prefix) / filename;
}

/// Own a temporary file's reserved directory, including on fatal test assertions.
class ScopedTempFile {
public:
    ScopedTempFile(std::string_view prefix, std::string_view suffix)
        : path_(UniqueTempFilePath(prefix, suffix)) {}
    ~ScopedTempFile() {
        std::error_code error;
        std::filesystem::remove_all(path_.parent_path(), error);
    }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&&) = delete;
    ScopedTempFile& operator=(ScopedTempFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const {
        return path_;
    }

private:
    const std::filesystem::path path_;
};

} // namespace Luminumbra::TestSupport
