#pragma once

// T-I3-17 COMPATIBILITY ALIAS — the engine implementation moved to
// src/luminumbra_common/fields/ScalarFieldDiffusion.{h,cpp}. "Aetheric" is
// Project Capture game flavor; this shim keeps the aetheric-named API and the
// game-flavored report schema alive for the existing diffusion gates.
// ALIAS REMOVAL AT ITERATION CLOSE (do not add new consumers).

#include "../fields/ScalarFieldDiffusion.h"

#include <string>

namespace luminumbra::aetheric {

using AethericDiffusionStep = fields::ScalarDiffusionStep;
using AethericDiffusionReport = fields::ScalarDiffusionReport;
using AethericFieldDiffusion = fields::ScalarFieldDiffusion;

// Forwarders: same fixture/gate/serializer as fields::, with the report
// restamped to the game-flavored schema and the serializer pointing at this
// compatibility shim (the artifact contract the legacy gates assert).
[[nodiscard]] AethericDiffusionReport RunAethericDiffusionFixture();
[[nodiscard]] bool AethericDiffusionMeetsGate(const AethericDiffusionReport& report) noexcept;
[[nodiscard]] std::string SerializeAethericDiffusionReportJson(const AethericDiffusionReport& report);

} // namespace luminumbra::aetheric
