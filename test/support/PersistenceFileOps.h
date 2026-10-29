#pragma once

#include <cstddef>
#include <filesystem>

// Injection for deterministic sharing-violation regressions. Install before
// starting threads and disarm after joining them. Read notifications bracket an
// actually open native stream. Publication is called under the exclusive lock,
// immediately before replace/delete; false emulates a failed filesystem call.
// Only tests declare this seam; it is absent from production headers.
namespace Luminumbra::Persistence::detail {
void set_file_read_observer_for_testing(void (*observer)(const std::filesystem::path&, bool));
void set_file_publication_hook_for_testing(bool (*hook)(const std::filesystem::path&, bool));
// Counts only publishers whose nonblocking lock acquisition already failed.
std::size_t waiting_file_publishers_for_testing(const std::filesystem::path& path);
} // namespace Luminumbra::Persistence::detail
