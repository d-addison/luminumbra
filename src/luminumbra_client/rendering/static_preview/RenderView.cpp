#include <luminumbra/rendering/RenderView.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Luminumbra::Rendering {
namespace {
void Require(bool condition, const char* diagnostic) {
    if (!condition)
        throw std::invalid_argument(diagnostic);
}

bool Close(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

std::array<float, 16> Packed(const glm::mat4& matrix) {
    std::array<float, 16> result;
    std::copy_n(glm::value_ptr(matrix), 16, result.begin());
    for (const float value : result)
        Require(std::isfinite(value), "Render view cannot be represented by the GPU matrix format");
    return result;
}
} // namespace

RenderView RenderView::Validate(const RenderViewDescription& description) {
    Require(description.revision != 0, "Render camera revision must be nonzero");
    Require(description.width > 0 && description.height > 0 && description.width <= 4096 &&
                description.height <= 4096,
            "Render extent must be between 1 and 4096 pixels on each axis");
    Require(std::isfinite(description.near_plane) && std::isfinite(description.far_plane) &&
                description.near_plane >= 0.001 && description.far_plane > description.near_plane &&
                description.far_plane <= 1e6,
            "Unsupported render camera near/far planes");
    for (const double value : description.view)
        Require(std::isfinite(value) && std::abs(value) <= 1e6,
                "Render view matrix is nonfinite or outside the static profile bounds");
    for (const double value : description.projection)
        Require(std::isfinite(value), "Render projection contains a nonfinite value");

    const glm::dmat4 view = glm::make_mat4(description.view.data());
    const glm::dmat4 projection = glm::make_mat4(description.projection.data());
    Require(view[0][3] == 0 && view[1][3] == 0 && view[2][3] == 0 && view[3][3] == 1,
            "Render view matrix must be affine");
    const glm::dmat3 rotation(view);
    const glm::dmat3 orthogonal = glm::transpose(rotation) * rotation;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            Require(Close(orthogonal[c][r], c == r ? 1 : 0), "Render view matrix must be rigid");
    Require(Close(glm::determinant(rotation), 1), "Render view matrix must preserve handedness");

    // Verify the provided matrix, preserving its exact entries. Orthographic
    // depth is affine; perspective retains w = -view_z. Do not reinterpret
    // hybrid/projective or shifted perspective matrices as orthographic.
    const bool orthographic = projection[2][3] == 0 && projection[3][3] == 1;
    glm::dmat4 expected(0.0);
    expected[0][0] = projection[0][0];
    expected[1][1] = projection[1][1];
    expected[2][2] = (orthographic ? 1.0 : description.near_plane) /
                     (description.far_plane - description.near_plane);
    expected[3][2] = description.far_plane * expected[2][2];
    if (orthographic) {
        expected[3][3] = 1;
        for (int axis = 0; axis < 2; ++axis) {
            Require(expected[axis][axis] >= static_cast<double>(1e-6f) &&
                        expected[axis][axis] <= 1000,
                    "Orthographic span must be between 0.002 and 2000000 metres");
            expected[3][axis] = projection[3][axis];
            Require(std::abs(expected[3][axis] / expected[axis][axis]) <= 1e6,
                    "Orthographic center exceeds the static profile bounds");
        }
    } else {
        expected[2][3] = -1;
        Require(expected[0][0] > 0.01 && expected[0][0] < 1000 && expected[1][1] > 0.01 &&
                    expected[1][1] < 1000,
                "Unsupported perspective field of view");
    }
    Require(Close(expected[1][1] / expected[0][0],
                  static_cast<double>(description.width) / description.height),
            "Render projection aspect differs from the output extent");
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            Require(std::abs(projection[c][r] - expected[c][r]) <=
                        std::max(1e-12, std::abs(expected[c][r]) * 2e-6),
                    "Expected finite reversed-Z perspective or orthographic projection");

    RenderView result;
    result.m_description = description;
    result.m_orthographic = orthographic;
    // Derive culling, eye and uniforms from the same matrices after conversion
    // to the actual rendering precision, so capture metadata can retain both.
    const glm::mat4 gpu_view(view);
    const glm::mat4 gpu_projection(projection);
    const glm::mat4 inverse_view = glm::inverse(gpu_view);
    const glm::mat4 view_projection = gpu_projection * gpu_view;
    result.m_view = Packed(gpu_view);
    result.m_projection = Packed(gpu_projection);
    result.m_inverse_view = Packed(inverse_view);
    result.m_view_projection = Packed(view_projection);
    for (int axis = 0; axis < 3; ++axis)
        result.m_eye[axis] = inverse_view[3][axis];

    const auto row = [&](int r) {
        return glm::vec4(view_projection[0][r],
                         view_projection[1][r],
                         view_projection[2][r],
                         view_projection[3][r]);
    };
    const std::array<glm::vec4, 6> planes{row(3) + row(0),
                                          row(3) - row(0),
                                          row(3) + row(1),
                                          row(3) - row(1),
                                          row(3) - row(2),
                                          row(2)};
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const float length = glm::length(glm::vec3(planes[i]));
        Require(std::isfinite(length) && length > 0, "Degenerate render culling plane");
        const glm::vec4 normalized = planes[i] / length;
        for (int axis = 0; axis < 4; ++axis) {
            Require(std::isfinite(normalized[axis]), "Nonfinite render culling plane");
            result.m_planes[i][axis] = normalized[axis];
        }
    }
    return result;
}
} // namespace Luminumbra::Rendering
