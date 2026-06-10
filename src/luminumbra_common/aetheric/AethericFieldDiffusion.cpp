#include "AethericFieldDiffusion.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace luminumbra::aetheric {
namespace {

constexpr double kConservationTolerance = 1.0e-9;
constexpr double kMaximumStableRate = 0.25;

[[nodiscard]] double clamp_unit(double value) noexcept
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

[[nodiscard]] bool finite(double value) noexcept
{
    return std::isfinite(value);
}

} // namespace

AethericFieldDiffusion::AethericFieldDiffusion(
    const std::size_t width,
    const std::size_t height,
    const double baseline_energy)
    : width_(width)
    , height_(height)
    , energy_(width * height, baseline_energy)
    , permeability_(width * height, 1.0)
    , sealed_(width * height, 0)
{
    if (width == 0 || height == 0) {
        throw std::invalid_argument("aetheric diffusion field dimensions must be non-zero");
    }
    if (!finite(baseline_energy) || baseline_energy < 0.0) {
        throw std::invalid_argument("aetheric diffusion baseline energy must be finite and non-negative");
    }
}

std::size_t AethericFieldDiffusion::width() const noexcept
{
    return width_;
}

std::size_t AethericFieldDiffusion::height() const noexcept
{
    return height_;
}

double AethericFieldDiffusion::at(const std::size_t x, const std::size_t y) const
{
    return energy_.at(index(x, y));
}

double AethericFieldDiffusion::total_energy() const noexcept
{
    double total = 0.0;
    for (const double value : energy_) {
        total += value;
    }
    return total;
}

double AethericFieldDiffusion::maximum_cell_energy() const noexcept
{
    double maximum = 0.0;
    for (const double value : energy_) {
        maximum = std::max(maximum, value);
    }
    return maximum;
}

const std::vector<double>& AethericFieldDiffusion::values() const noexcept
{
    return energy_;
}

void AethericFieldDiffusion::set(const std::size_t x, const std::size_t y, const double energy)
{
    if (!finite(energy) || energy < 0.0) {
        throw std::invalid_argument("aetheric cell energy must be finite and non-negative");
    }
    energy_.at(index(x, y)) = energy;
}

void AethericFieldDiffusion::add_impulse(const std::size_t x, const std::size_t y, const double energy)
{
    if (!finite(energy) || energy < 0.0) {
        throw std::invalid_argument("aetheric impulse energy must be finite and non-negative");
    }
    energy_.at(index(x, y)) += energy;
}

void AethericFieldDiffusion::set_permeability(
    const std::size_t x,
    const std::size_t y,
    const double permeability)
{
    if (!finite(permeability)) {
        throw std::invalid_argument("aetheric permeability must be finite");
    }
    permeability_.at(index(x, y)) = clamp_unit(permeability);
}

void AethericFieldDiffusion::seal(const std::size_t x, const std::size_t y, const bool sealed)
{
    sealed_.at(index(x, y)) = sealed ? 1 : 0;
}

AethericDiffusionReport AethericFieldDiffusion::diffuse(
    const std::size_t iterations,
    const double diffusion_rate)
{
    if (!finite(diffusion_rate) || diffusion_rate <= 0.0 || diffusion_rate > kMaximumStableRate) {
        throw std::invalid_argument("aetheric diffusion rate must be finite and in the stable range (0, 0.25]");
    }

    AethericDiffusionReport report;
    report.width = width_;
    report.height = height_;
    report.iterations = iterations;
    report.initial_energy = total_energy();
    report.steps.reserve(iterations);

    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        std::vector<double> next = energy_;
        double max_delta = 0.0;

        const auto exchange = [&](const std::size_t lhs, const std::size_t rhs) {
            if (!can_exchange(lhs, rhs)) {
                return;
            }

            const double conductance = edge_conductance(lhs, rhs);
            const double delta = (energy_[lhs] - energy_[rhs]) * diffusion_rate * conductance;
            next[lhs] -= delta;
            next[rhs] += delta;
            max_delta = std::max(max_delta, std::abs(delta));
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

        report.steps.push_back(AethericDiffusionStep{
            iteration + 1,
            total,
            conservation_error,
            max_delta,
            conservation_error <= kConservationTolerance && finite(max_delta),
        });
    }

    report.final_energy = total_energy();
    report.conservation_error = std::abs(report.final_energy - report.initial_energy);
    report.maximum_cell_energy = maximum_cell_energy();
    report.passed = AethericDiffusionMeetsGate(report);
    return report;
}

std::size_t AethericFieldDiffusion::index(const std::size_t x, const std::size_t y) const
{
    if (x >= width_ || y >= height_) {
        throw std::out_of_range("aetheric field coordinate out of range");
    }
    return y * width_ + x;
}

bool AethericFieldDiffusion::can_exchange(const std::size_t lhs, const std::size_t rhs) const noexcept
{
    return sealed_[lhs] == 0 && sealed_[rhs] == 0 && edge_conductance(lhs, rhs) > 0.0;
}

double AethericFieldDiffusion::edge_conductance(const std::size_t lhs, const std::size_t rhs) const noexcept
{
    return std::min(permeability_[lhs], permeability_[rhs]);
}

AethericDiffusionReport RunAethericDiffusionFixture()
{
    AethericFieldDiffusion field(5, 5, 0.0);
    field.add_impulse(2, 2, 16.0);
    field.add_impulse(1, 2, 2.0);
    field.add_impulse(3, 2, 2.0);

    field.set_permeability(0, 2, 0.35);
    field.set_permeability(4, 2, 0.35);
    field.set_permeability(2, 0, 0.50);
    field.set_permeability(2, 4, 0.50);
    field.seal(0, 0);
    field.seal(4, 4);

    return field.diffuse(10, 0.125);
}

bool AethericDiffusionMeetsGate(const AethericDiffusionReport& report) noexcept
{
    if (report.schema != "luminumbra.aetheric.field_diffusion.v1") {
        return false;
    }
    if (report.width == 0 || report.height == 0 || report.iterations < 8) {
        return false;
    }
    if (report.steps.size() != report.iterations) {
        return false;
    }
    if (!finite(report.initial_energy) || report.initial_energy <= 0.0 ||
        !finite(report.final_energy) || !finite(report.conservation_error) ||
        !finite(report.maximum_cell_energy)) {
        return false;
    }
    if (report.conservation_error > kConservationTolerance) {
        return false;
    }
    if (report.maximum_cell_energy >= report.initial_energy * 0.85) {
        return false;
    }

    for (const AethericDiffusionStep& step : report.steps) {
        if (!step.stable || !finite(step.total_energy) || !finite(step.max_delta) ||
            step.conservation_error > kConservationTolerance) {
            return false;
        }
    }

    return true;
}

std::string SerializeAethericDiffusionReportJson(const AethericDiffusionReport& report)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(10);
    out << "{\n";
    out << "  \"schema\": \"" << report.schema << "\",\n";
    out << "  \"passed\": " << (report.passed ? "true" : "false") << ",\n";
    out << "  \"field\": {\n";
    out << "    \"source\": \"src/luminumbra_common/aetheric/AethericFieldDiffusion.cpp\",\n";
    out << "    \"header\": \"src/luminumbra_common/aetheric/AethericFieldDiffusion.h\",\n";
    out << "    \"width\": " << report.width << ",\n";
    out << "    \"height\": " << report.height << ",\n";
    out << "    \"cell_count\": " << (report.width * report.height) << ",\n";
    out << "    \"boundary\": \"sealed_edges\",\n";
    out << "    \"permeability_model\": \"per_cell_min_edge\"\n";
    out << "  },\n";
    out << "  \"diffusion\": {\n";
    out << "    \"solver\": \"conservative_pairwise_flux\",\n";
    out << "    \"order_contract\": \"deterministic_row_major_edges\",\n";
    out << "    \"iterations\": " << report.iterations << ",\n";
    out << "    \"initial_energy\": " << report.initial_energy << ",\n";
    out << "    \"final_energy\": " << report.final_energy << ",\n";
    out << "    \"conservation_error\": " << report.conservation_error << ",\n";
    out << "    \"maximum_cell_energy\": " << report.maximum_cell_energy << ",\n";
    out << "    \"stable\": " << (report.passed ? "true" : "false") << "\n";
    out << "  },\n";
    out << "  \"steps\": [\n";
    for (std::size_t i = 0; i < report.steps.size(); ++i) {
        const AethericDiffusionStep& step = report.steps[i];
        out << "    { \"iteration\": " << step.iteration
            << ", \"total_energy\": " << step.total_energy
            << ", \"conservation_error\": " << step.conservation_error
            << ", \"max_delta\": " << step.max_delta
            << ", \"stable\": " << (step.stable ? "true" : "false") << " }";
        out << (i + 1 < report.steps.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

} // namespace luminumbra::aetheric
