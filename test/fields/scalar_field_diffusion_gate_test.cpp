#include "luminumbra_common/fields/ScalarFieldDiffusion.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace {

void require_gate(const bool condition) {
    if (!condition) {
        std::abort();
    }
}

// the game-flavored "aetheric" compatibility alias was removed at
// iteration close. The generic engine fields::ScalarFieldDiffusion solver is
// now exercised directly under its own schema.
const bool kScalarFieldDiffusionGate = [] {
    const luminumbra::fields::ScalarDiffusionReport report =
        luminumbra::fields::RunScalarDiffusionFixture();

    require_gate(luminumbra::fields::ScalarDiffusionMeetsGate(report));
    require_gate(report.schema == "luminumbra.fields.scalar_diffusion.v1");
    require_gate(report.width == 5);
    require_gate(report.height == 5);
    require_gate(report.iterations == 10);
    require_gate(report.steps.size() == report.iterations);
    require_gate(std::abs(report.initial_energy - report.final_energy) <= 1.0e-9);
    require_gate(report.maximum_cell_energy < report.initial_energy);

    const std::string json = luminumbra::fields::SerializeScalarDiffusionReportJson(report);
    require_gate(json.find("conservative_pairwise_flux") != std::string::npos);
    require_gate(json.find("deterministic_row_major_edges") != std::string::npos);
    require_gate(json.find("src/luminumbra_common/fields/ScalarFieldDiffusion.cpp") !=
                 std::string::npos);

    return true;
}();

} // namespace
