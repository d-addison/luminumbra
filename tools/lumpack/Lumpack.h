#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>

namespace Luminumbra::Lumpack {

// Arguments exclude the executable name: validate <mod-dir> [<mod-dir> ...].
// Emits one JSON object, with diagnostics sorted by UTF-8 path, code and message.
// Returns 0 for valid input, 1 for invalid input or output failure, 2 for usage.
int Run(std::span<const std::filesystem::path> arguments, std::ostream& output);

} // namespace Luminumbra::Lumpack
