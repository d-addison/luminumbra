#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace luminumbra::aetheric {

struct AethericDiffusionStep {
    std::size_t iteration = 0;
    double total_energy = 0.0;
    double conservation_error = 0.0;
    double max_delta = 0.0;
    bool stable = false;
};

struct AethericDiffusionReport {
    std::string schema = "luminumbra.aetheric.field_diffusion.v1";
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t iterations = 0;
    double initial_energy = 0.0;
    double final_energy = 0.0;
    double conservation_error = 0.0;
    double maximum_cell_energy = 0.0;
    bool passed = false;
    std::vector<AethericDiffusionStep> steps;
};

class AethericFieldDiffusion {
public:
    AethericFieldDiffusion(std::size_t width, std::size_t height, double baseline_energy = 0.0);

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

    [[nodiscard]] AethericDiffusionReport diffuse(std::size_t iterations, double diffusion_rate);

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

[[nodiscard]] AethericDiffusionReport RunAethericDiffusionFixture();
[[nodiscard]] bool AethericDiffusionMeetsGate(const AethericDiffusionReport& report) noexcept;
[[nodiscard]] std::string SerializeAethericDiffusionReportJson(const AethericDiffusionReport& report);

} // namespace luminumbra::aetheric
