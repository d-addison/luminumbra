#include "../support/RegionalFieldEvidence.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        if (argc != 2 || (std::string_view(argv[1]) != "--diagnostic-pressure" &&
                          std::string_view(argv[1]) != "--shipped-settings"))
            throw std::invalid_argument("choose --diagnostic-pressure or --shipped-settings");
        Luminumbra::TestSupport::RegionalFieldEvidence driver(std::string_view(argv[1]) ==
                                                              "--diagnostic-pressure");
        const auto packet = driver.Capture(20000, 128);
        std::cout << packet.dump() << '\n';
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("cannot write field evidence packet");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
