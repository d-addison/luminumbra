#include "../support/RegionalFieldEvidence.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const auto options = Luminumbra::TestSupport::ParseRegionalFieldEvidenceOptions(argc, argv);
        Luminumbra::TestSupport::RegionalFieldEvidence driver(options.pressure);
        const auto packet = driver.Capture(20000, 128);
        Luminumbra::TestSupport::WriteRegionalFieldEvidence(options.output, packet);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
