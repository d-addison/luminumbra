#include "CommonLibraryIsa.h"

#include <cstdio>

int LuminumbraApplicationMain(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    if (!Luminumbra::Startup::SupportsCommonLibrary()) {
        std::fprintf(stderr, "%s\n", Luminumbra::Startup::kUnsupportedCommonIsa);
        return 1;
    }
    return LuminumbraApplicationMain(argc, argv);
}
