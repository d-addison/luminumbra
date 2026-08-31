#pragma once
// Shared, -free description of the dual-backend FLIP calibration pass
// (  / + ). The raw-GL golden (glad, in
// dual_backend_flip_test.cpp) and the GL-via-Diligent candidate
// (dual_backend_diligent.cpp, the only Diligent-including render TU) MUST feed
// byte-identical geometry + camera + light so a non-zero in-process FLIP score is
// attributable to the BACKEND, never to differing inputs. This header is that
// single source of truth: pure POD + std, no glad, no Diligent -> safe to include
// from both the glad TU and the Diligent TU without header conflicts.

#include <array>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

namespace luminumbra_test {

// The calibration resolution recorded in dual_backend_flip.json.
inline constexpr int kCubeWidth = 256;
inline constexpr int kCubeHeight = 256;

// Non-sRGB clear colour; its RGBA8 quantisation (10,15,20) is the "background"
// sentinel the golden test checks. Both backends must clear to these exact bytes.
inline constexpr float kClearR = 0.04f;
inline constexpr float kClearG = 0.06f;
inline constexpr float kClearB = 0.08f;

struct MeshVertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct RenderParams {
    glm::vec3 object_color{0.44f, 0.72f, 0.38f};
    glm::vec3 camera_position{3.2f, 2.6f, 4.4f};
    glm::vec3 camera_target{0.0f, 0.0f, 0.0f};
    glm::vec3 light_pos{6.0f, 8.0f, 5.0f};
};

// A deterministic unit cube with per-face normals: three faces visible from the
// camera give distinct diffuse levels plus silhouette edges against the clear
// colour, so the metric's luma, gradient, and colour terms are all exercised.
inline std::vector<MeshVertex> BuildCubeMesh() {
    constexpr float h = 1.2f;
    struct Face {
        glm::vec3 normal;
        std::array<glm::vec3, 4> corners;
    };
    const std::array<Face, 6> faces = {{
        {{1, 0, 0}, {{{h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}}}},
        {{-1, 0, 0}, {{{-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}}}},
        {{0, 1, 0}, {{{-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}}}},
        {{0, -1, 0}, {{{-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}}}},
        {{0, 0, 1}, {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}}},
        {{0, 0, -1}, {{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}}},
    }};
    std::vector<MeshVertex> verts;
    verts.reserve(faces.size() * 6);
    for (const Face& f : faces) {
        const std::array<int, 6> order = {0, 1, 2, 0, 2, 3};
        for (int idx : order) {
            const glm::vec3& c = f.corners[static_cast<std::size_t>(idx)];
            verts.push_back({c.x, c.y, c.z, f.normal.x, f.normal.y, f.normal.z});
        }
    }
    return verts;
}

// Vertically flip an RGBA8 image in place-returning form. GL glReadPixels yields
// bottom-up rows; Diligent normalises texture readback to top-left origin. This is
// the ONE convention difference between the two backends -- exposing it (rather
// than hiding it) is why leg B measures BOTH orientations and reports each score.
inline std::vector<unsigned char>
FlipRowsVertically(const std::vector<unsigned char>& src, int width, int height) {
    std::vector<unsigned char> out(src.size());
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    for (int y = 0; y < height; ++y) {
        const std::size_t src_row = static_cast<std::size_t>(y) * row_bytes;
        const std::size_t dst_row = static_cast<std::size_t>(height - 1 - y) * row_bytes;
        if (src_row + row_bytes <= src.size() && dst_row + row_bytes <= out.size()) {
            std::copy(src.begin() + static_cast<std::ptrdiff_t>(src_row),
                      src.begin() + static_cast<std::ptrdiff_t>(src_row + row_bytes),
                      out.begin() + static_cast<std::ptrdiff_t>(dst_row));
        }
    }
    return out;
}

} // namespace luminumbra_test
