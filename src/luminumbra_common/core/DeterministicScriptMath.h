#pragma once

// Binary64 extension of DeterministicMath; existing binary32 results are unchanged.
// Requires round-to-nearest, gradual underflow, and the common target's pinned
// floating-point contract (no reassociation or fused multiply-add contraction).
// Domain errors and NaN inputs produce the canonical quiet NaN 0x7ff8000000000000.
// Signed zeros and infinities follow IEEE arithmetic. No errno contract is exposed.
namespace Luminumbra::DeterministicMath::Script {

[[nodiscard]] double Sin(double value) noexcept;
[[nodiscard]] double Cos(double value) noexcept;
[[nodiscard]] double Tan(double value) noexcept;
[[nodiscard]] double Atan2(double ordinate, double abscissa) noexcept;
[[nodiscard]] double Pow(double base, double exponent) noexcept;
[[nodiscard]] double Log(double value) noexcept;
[[nodiscard]] double Exp(double value) noexcept;
[[nodiscard]] double Sqrt(double value) noexcept;

} // namespace Luminumbra::DeterministicMath::Script

namespace Luminumbra {
namespace DeterministicScriptMath = DeterministicMath::Script;
}
