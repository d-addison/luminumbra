#pragma once

#include <array>
#include <cstdint>

namespace Luminumbra::Rendering {

// Column-major matrices, right-handed world/view coordinates, metres. This
// public value has no graphics API, window, game-camera or editor dependency.
using ViewMatrix = std::array<double, 16>;

struct RenderViewDescription {
    ViewMatrix view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ViewMatrix projection{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t revision = 0;
    double near_plane = 0.1;
    double far_plane = 1000.0;
};

// The initial static inspection profile accepts finite symmetric perspective
// projections with zero-to-one reversed depth (near 1, far/clear 0), rigid view
// matrices, one output extent and no jitter. Unsupported projections throw;
// neither the renderer nor culling may silently reconstruct a different camera.
class RenderView {
public:
    static RenderView Validate(const RenderViewDescription& description);

    const RenderViewDescription& description() const {
        return m_description;
    }
    const std::array<float, 16>& view() const {
        return m_view;
    }
    const std::array<float, 16>& projection() const {
        return m_projection;
    }
    const std::array<float, 16>& inverse_view() const {
        return m_inverse_view;
    }
    const std::array<float, 16>& view_projection() const {
        return m_view_projection;
    }
    const std::array<float, 3>& eye() const {
        return m_eye;
    }
    const std::array<std::array<float, 4>, 6>& frustum_planes() const {
        return m_planes;
    }

private:
    RenderView() = default;
    RenderViewDescription m_description;
    std::array<float, 16> m_view{};
    std::array<float, 16> m_projection{};
    std::array<float, 16> m_inverse_view{};
    std::array<float, 16> m_view_projection{};
    std::array<float, 3> m_eye{};
    std::array<std::array<float, 4>, 6> m_planes{};
};

} // namespace Luminumbra::Rendering
