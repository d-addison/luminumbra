#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Luminumbra::Authoring::Detail {
inline void Require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

inline bool Identifier(const std::string& value) {
    const auto alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (value.empty() || value.size() > 128 || !alpha(value[0]))
        return false;
    for (char c : value)
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '_' && c != '.' && c != ':' && c != '-')
            return false;
    return true;
}

inline bool Hex(const std::string& value, std::size_t length) {
    if (value.size() != length)
        return false;
    for (char c : value)
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
            return false;
    return true;
}

inline void Affine(const glm::dmat4& matrix) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            Require(std::isfinite(matrix[c][r]) && std::abs(matrix[c][r]) <= 1e30,
                    "Prefab transform is nonfinite or outside the supported range");
    Require(matrix[0][3] == 0 && matrix[1][3] == 0 && matrix[2][3] == 0 && matrix[3][3] == 1,
            "Prefab transform must be affine");
    Require(std::abs(glm::determinant(glm::dmat3(matrix))) > 1e-12, "Prefab transform is singular");
    const auto normal = glm::transpose(glm::inverse(glm::dmat3(matrix)));
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            Require(std::isfinite(normal[c][r]), "Prefab normal transform is nonfinite");
}

inline nlohmann::json MatrixJson(const glm::dmat4& matrix) {
    auto result = nlohmann::json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            result.push_back(matrix[c][r]);
    return result;
}
} // namespace Luminumbra::Authoring::Detail
