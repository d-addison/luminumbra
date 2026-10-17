#pragma once

// Runtime-root resolution for the client app, extracted verbatim from
// main_client.cpp. Walks the cwd/exe ancestor directories to find the first
// directory that actually holds the runtime assets (shaders, audio banks,
// world presets), so the client boots correctly from a build tree, an
// installed layout, or a packaging staging dir.

#include <filesystem>
#include <string>

namespace Luminumbra::Client::App {

// An explicit root bypasses discovery and throws on invalid or incomplete layouts.
std::filesystem::path ResolveRuntimeRoot(const char* argv0,
                                         const std::filesystem::path& explicit_root = {});
std::string RuntimeRootString(const std::filesystem::path& root_dir);

} // namespace Luminumbra::Client::App
