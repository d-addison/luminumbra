// T-I3-17 COMPATIBILITY ALIAS — see the header. The solver lives in
// src/luminumbra_common/fields/ScalarFieldDiffusion.cpp; these forwarders
// keep the aetheric-named gate API and the game-flavored schema stable until
// the alias is removed at iteration close.

#include "AethericFieldDiffusion.h"

namespace luminumbra::aetheric {

namespace {
constexpr const char* kAethericSchema = "luminumbra.aetheric.field_diffusion.v1";
} // namespace

AethericDiffusionReport RunAethericDiffusionFixture()
{
    AethericDiffusionReport report = fields::RunScalarDiffusionFixture();
    report.schema = kAethericSchema;
    report.passed = AethericDiffusionMeetsGate(report);
    return report;
}

bool AethericDiffusionMeetsGate(const AethericDiffusionReport& report) noexcept
{
    // Dual-schema acceptance (T-I3-17): the alias accepts both the
    // game-flavored schema and the engine schema, restamping to the engine
    // schema before delegating so the engine gate stays single-schema.
    if (report.schema != kAethericSchema &&
        report.schema != fields::kScalarDiffusionSchema) {
        return false;
    }
    AethericDiffusionReport restamped = report;
    restamped.schema = fields::kScalarDiffusionSchema;
    return fields::ScalarDiffusionMeetsGate(restamped);
}

std::string SerializeAethericDiffusionReportJson(const AethericDiffusionReport& report)
{
    return fields::SerializeScalarDiffusionReportJson(
        report,
        "src/luminumbra_common/aetheric/AethericFieldDiffusion.cpp",
        "src/luminumbra_common/aetheric/AethericFieldDiffusion.h");
}

} // namespace luminumbra::aetheric
