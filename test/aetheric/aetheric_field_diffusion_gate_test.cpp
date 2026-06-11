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
    // Compatibility alias path (T-I3-17): the aetheric-named API forwards to
    // fields/ScalarFieldDiffusion with the game-flavored schema restamped.
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

    // Engine path parity: the relocated fields::ScalarFieldDiffusion fixture
    // produces bitwise-identical numbers under its own schema (dual-schema
    // acceptance window).
    const luminumbra::fields::ScalarDiffusionReport fields_report =
        luminumbra::fields::RunScalarDiffusionFixture();
    require_gate(fields_report.schema == "luminumbra.fields.scalar_diffusion.v1");
    require_gate(luminumbra::fields::ScalarDiffusionMeetsGate(fields_report));
    require_gate(fields_report.final_energy == report.final_energy);
    require_gate(fields_report.maximum_cell_energy == report.maximum_cell_energy);
    require_gate(fields_report.steps.size() == report.steps.size());
    for (std::size_t i = 0; i < fields_report.steps.size(); ++i) {
        require_gate(fields_report.steps[i].total_energy == report.steps[i].total_energy);
        require_gate(fields_report.steps[i].max_delta == report.steps[i].max_delta);
    }
    const std::string fields_json =
        luminumbra::fields::SerializeScalarDiffusionReportJson(fields_report);
    require_gate(fields_json.find("src/luminumbra_common/fields/ScalarFieldDiffusion.cpp") != std::string::npos);

    return true;
}();

} // namespace
