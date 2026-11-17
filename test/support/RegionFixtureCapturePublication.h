#pragma once

#include "UniqueTempPath.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Luminumbra::TestSupport {

inline void PublishCapture(const std::filesystem::path& destination, const std::string& payload) {
    // Stage on the destination filesystem; close before publishing on Windows.
    std::error_code error;
    const auto staging = Luminumbra::TestSupport::detail::ReserveTempDirectory(
        destination.parent_path(),
        Luminumbra::TestSupport::detail::UniqueTempStem("region_capture_"),
        error);
    if (staging.empty()) {
        throw std::filesystem::filesystem_error("cannot stage capture", destination, error);
    }
    try {
        const auto temporary = staging / "capture.json";
        std::ofstream output(temporary, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << payload;
        output.close();
        // The publication operation itself refuses existing names, including a
        // destination created by another capture while these bytes were staged.
#if defined(_WIN32)
        if (!::MoveFileExW(temporary.c_str(), destination.c_str(), 0)) {
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        }
#else
        std::filesystem::create_hard_link(temporary, destination, error);
#endif
        if (error) {
            throw std::filesystem::filesystem_error(
                "cannot publish capture: destination must be new", temporary, destination, error);
        }
    } catch (...) {
        std::filesystem::remove_all(staging, error);
        throw;
    }
    std::filesystem::remove_all(staging);
}

} // namespace Luminumbra::TestSupport
