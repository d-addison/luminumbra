#include "luminumbra_common/aetheric/AethericFieldDiffusion.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace {

void require_gate(const bool condition)
{
    if (!condition) {
        std::abort();
    }
}

const bool kAethericFieldDiffusionGate = [] {
    const luminumbra::aetheric::AethericDiffusionReport report =
        luminumbra::aetheric::RunAethericDiffusionFixture();

    require_gate(luminumbra::aetheric::AethericDiffusionMeetsGate(report));
    require_gate(report.schema == "luminumbra.aetheric.field_diffusion.v1");
    require_gate(report.width == 5);
    require_gate(report.height == 5);
    require_gate(report.iterations == 10);
    require_gate(report.steps.size() == report.iterations);
    require_gate(std::abs(report.initial_energy - report.final_energy) <= 1.0e-9);
    require_gate(report.maximum_cell_energy < report.initial_energy);

    const std::string json = luminumbra::aetheric::SerializeAethericDiffusionReportJson(report);
    require_gate(json.find("conservative_pairwise_flux") != std::string::npos);
    require_gate(json.find("deterministic_row_major_edges") != std::string::npos);
    require_gate(json.find("src/luminumbra_common/aetheric/AethericFieldDiffusion.cpp") != std::string::npos);

    return true;
}();

} // namespace
