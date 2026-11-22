#include "Lumpack.h"

#include "luminumbra_common/mods/ModManifest.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
#if defined(_WIN32) && !defined(__CYGWIN__)
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            return 1;
        }
#endif
        // Refuse excessive argument counts before allocating argument storage.
        if (argc < 3 || argc - 2 > static_cast<int>(luminumbra::mods::kMaxModCount)) {
            return Luminumbra::Lumpack::Run({}, std::cout);
        }
        std::vector<std::filesystem::path> arguments;
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        return Luminumbra::Lumpack::Run(arguments, std::cout);
    } catch (const std::exception&) {
        return 1;
    }
}
