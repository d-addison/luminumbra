#pragma once

// generic conservative scalar-field diffusion (engine). Relocated
// from the game-flavored field-diffusion module; the legacy game-named
// compatibility alias forwards here (alias removal at iteration close). The
// solver, fixtures, and contracts are unchanged.

#include <cstddef>
#include <string>
#include <vector>

namespace luminumbra::fields {

inline constexpr const char* kScalarDiffusionSchema = "luminumbra.fields.scalar_diffusion.v1";

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

private:
    [[nodiscard]] std::size_t index(std::size_t x, std::size_t y) const;
    [[nodiscard]] bool can_exchange(std::size_t lhs, std::size_t rhs) const noexcept;
    [[nodiscard]] double edge_conductance(std::size_t lhs, std::size_t rhs) const noexcept;

    std::size_t width_ = 0;
    std::size_t height_ = 0;
    std::vector<double> energy_;
    std::vector<double> permeability_;
    std::vector<unsigned char> sealed_;
};

[[nodiscard]] ScalarDiffusionReport RunScalarDiffusionFixture();
// Accepts the engine schema; the compatibility alias restamps its schema
// before delegating here (dual-schema acceptance lives in the alias).
[[nodiscard]] bool ScalarDiffusionMeetsGate(const ScalarDiffusionReport& report) noexcept;
[[nodiscard]] std::string SerializeScalarDiffusionReportJson(
    const ScalarDiffusionReport& report,
    const std::string& source_path = "src/luminumbra_common/fields/ScalarFieldDiffusion.cpp",
    const std::string& header_path = "src/luminumbra_common/fields/ScalarFieldDiffusion.h");

} // namespace luminumbra::fields
