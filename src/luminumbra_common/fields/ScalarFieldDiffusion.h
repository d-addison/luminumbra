#pragma once

// generic conservative scalar-field diffusion (engine). Relocated
// from the game-flavored field-diffusion module; the legacy game-named
// compatibility alias forwards here (alias removal at iteration close). The
// solver, fixtures, and contracts are unchanged.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace luminumbra::fields {

inline constexpr const char* kScalarDiffusionSchema = "luminumbra.fields.scalar_diffusion.v1";

// Only nullable gate forms can be empty. Any other callable is always invoked, even
// when it also converts to bool, so a false conversion never opens an edge or cell.
template<typename Gate>
struct IsStdFunctionGate : std::false_type {};
template<typename Signature>
struct IsStdFunctionGate<std::function<Signature>> : std::true_type {};
template<typename Gate>
inline constexpr bool kNullableGate =
    std::is_pointer_v<std::remove_cvref_t<Gate>> ||
    IsStdFunctionGate<std::remove_cvref_t<Gate>>::value ||
    requires { typename std::remove_cvref_t<Gate>::nullable_gate; };

struct ScalarDiffusionStep {
    std::size_t iteration = 0;
    double total_energy = 0.0;
    double conservation_error = 0.0;
    double max_delta = 0.0;
    bool stable = false;
};

struct ScalarDiffusionReport {
    std::string schema = kScalarDiffusionSchema;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t iterations = 0;
    double initial_energy = 0.0;
    double final_energy = 0.0;
    double conservation_error = 0.0;
    double maximum_cell_energy = 0.0;
    bool passed = false;
    std::vector<ScalarDiffusionStep> steps;
};

// Accepts the engine schema; the compatibility alias restamps its schema
// before delegating here (dual-schema acceptance lives in the alias).
[[nodiscard]] bool ScalarDiffusionMeetsGate(const ScalarDiffusionReport& report) noexcept;

class ScalarFieldDiffusion {
public:
    ScalarFieldDiffusion(std::size_t width, std::size_t height, double baseline_energy = 0.0);

    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;
    [[nodiscard]] double at(std::size_t x, std::size_t y) const;
    [[nodiscard]] double total_energy() const noexcept;
    [[nodiscard]] double maximum_cell_energy() const noexcept;
    [[nodiscard]] const std::vector<double>& values() const noexcept;

    void set(std::size_t x, std::size_t y, double energy);
    void add_impulse(std::size_t x, std::size_t y, double energy);
    void set_permeability(std::size_t x, std::size_t y, double permeability);
    void seal(std::size_t x, std::size_t y, bool sealed = true);

    [[nodiscard]] ScalarDiffusionReport diffuse(std::size_t iterations, double diffusion_rate);
    // Same solver, but an edge also needs `admits(lhs, rhs)` (row-major cell
    // indices) to exchange. Intermediate cells are stored exactly as in diffuse.
    using EdgeFilter = std::function<bool(std::size_t lhs, std::size_t rhs)>;
    [[nodiscard]] ScalarDiffusionReport
    diffuse(std::size_t iterations, double diffusion_rate, const EdgeFilter& admits);

    // Coordinate gates use the same edge order and admission rules as index filters.
    using EdgeGate = std::function<bool(std::size_t, std::size_t, std::size_t, std::size_t)>;
    [[nodiscard]] ScalarDiffusionReport
    diffuse(std::size_t iterations, double diffusion_rate, const EdgeGate& gate);
    // An explicit empty argument selects the unfiltered solver unambiguously.
    [[nodiscard]] ScalarDiffusionReport
    diffuse(std::size_t iterations, double diffusion_rate, std::nullptr_t);

    // Concrete index and coordinate gates retain their type throughout the edge loop.
    template<typename Gate>
        requires(std::is_invocable_r_v<bool, const Gate&, std::size_t, std::size_t> ||
                 std::is_invocable_r_v<bool,
                                       const Gate&,
                                       std::size_t,
                                       std::size_t,
                                       std::size_t,
                                       std::size_t>)
    [[nodiscard]] ScalarDiffusionReport
    diffuse(std::size_t iterations, double diffusion_rate, const Gate& admits) {
        if (!std::isfinite(diffusion_rate) || diffusion_rate <= 0.0 || diffusion_rate > 0.25) {
            throw std::invalid_argument(
                "scalar diffusion rate must be finite and in the stable range (0, 0.25]");
        }

        ScalarDiffusionReport report;
        report.width = width_;
        report.height = height_;
        report.iterations = iterations;
        report.initial_energy = total_energy();
        report.steps.reserve(iterations);

        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            std::vector<double> next = energy_;
            double max_delta = 0.0;

            const auto admitted = [&](const std::size_t lhs, const std::size_t rhs) -> bool {
                if constexpr (kNullableGate<Gate>) {
                    if (!admits) {
                        return true;
                    }
                }
                if constexpr (std::is_invocable_r_v<bool, const Gate&, std::size_t, std::size_t>) {
                    return static_cast<bool>(admits(lhs, rhs));
                } else {
                    return static_cast<bool>(
                        admits(lhs % width_, lhs / width_, rhs % width_, rhs / width_));
                }
            };
            const auto exchange = [&](const std::size_t lhs, const std::size_t rhs) {
                if (!can_exchange(lhs, rhs) || !admitted(lhs, rhs)) {
                    return;
                }

                const double conductance = edge_conductance(lhs, rhs);
                const double delta = (energy_[lhs] - energy_[rhs]) * diffusion_rate * conductance;
                next[lhs] -= delta;
                next[rhs] += delta;
                max_delta = (std::max)(max_delta, std::abs(delta));
            };

            for (std::size_t y = 0; y < height_; ++y) {
                for (std::size_t x = 0; x + 1 < width_; ++x) {
                    exchange(index(x, y), index(x + 1, y));
                }
            }
            for (std::size_t y = 0; y + 1 < height_; ++y) {
                for (std::size_t x = 0; x < width_; ++x) {
                    exchange(index(x, y), index(x, y + 1));
                }
            }

            energy_ = std::move(next);
            const double total = total_energy();
            const double conservation_error = std::abs(total - report.initial_energy);

            report.steps.push_back(ScalarDiffusionStep{
                iteration + 1,
                total,
                conservation_error,
                max_delta,
                conservation_error <= 1.0e-9 && std::isfinite(max_delta),
            });
        }

        report.final_energy = total_energy();
        report.conservation_error = std::abs(report.final_energy - report.initial_energy);
        report.maximum_cell_energy = maximum_cell_energy();
        report.passed = ScalarDiffusionMeetsGate(report);
        return report;
    }

private:
    [[nodiscard]] std::size_t index(const std::size_t x, const std::size_t y) const {
        if (x >= width_ || y >= height_) {
            throw std::out_of_range("scalar field coordinate out of range");
        }
        return y * width_ + x;
    }
    [[nodiscard]] bool can_exchange(const std::size_t lhs, const std::size_t rhs) const noexcept {
        return sealed_[lhs] == 0 && sealed_[rhs] == 0 && edge_conductance(lhs, rhs) > 0.0;
    }
    [[nodiscard]] double edge_conductance(const std::size_t lhs,
                                          const std::size_t rhs) const noexcept {
        return (std::min)(permeability_[lhs], permeability_[rhs]);
    }

    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::vector<double> energy_;
    std::vector<double> permeability_;
    std::vector<unsigned char> sealed_;
};

[[nodiscard]] ScalarDiffusionReport RunScalarDiffusionFixture();
[[nodiscard]] std::string SerializeScalarDiffusionReportJson(
    const ScalarDiffusionReport& report,
    const std::string& source_path = "src/luminumbra_common/fields/ScalarFieldDiffusion.cpp",
    const std::string& header_path = "src/luminumbra_common/fields/ScalarFieldDiffusion.h");

} // namespace luminumbra::fields
