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
    const auto stem = detail::UniqueTempStem(prefix);
    for (int attempt = 0; attempt < detail::kTempPathAttempts; ++attempt) {
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

/// Return an absent file path without creating it, retaining the supplied suffix.
/// The case name separates discovered tests and the counter separates calls.
/// This is not an exclusive reservation against concurrent runs of the same case;
/// callers needing that guarantee should put their file in CreateUniqueTempDirectory.
[[nodiscard]] inline std::filesystem::path UniqueTempFilePath(std::string_view prefix,
                                                              std::string_view suffix) {
    const auto temporary_root = std::filesystem::temp_directory_path();
    const auto stem = detail::UniqueTempStem(prefix);
    std::error_code error;
    for (int attempt = 0; attempt < detail::kTempPathAttempts; ++attempt) {
        const auto candidate =
            temporary_root / (stem + "_" + std::to_string(attempt) + std::string(suffix));
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (status.type() == std::filesystem::file_type::not_found) {
            return candidate;
        }
        if (!error) {
            error = std::make_error_code(std::errc::file_exists);
        }
    }
    throw std::runtime_error("Failed to find an absent temporary file path: " + error.message());
}

} // namespace Luminumbra::TestSupport
