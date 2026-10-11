#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {
class RenderView;
struct StaticDrawSnapshot;
struct GBuffer;

struct StaticDrawStats {
    std::size_t draws = 0, indices = 0;
    std::size_t uploaded_meshes = 0, uploaded_textures = 0;
    std::size_t updated_instances = 0;
};

// Production material-aware static geometry stage. Draws into the existing
// deferred targets; both game composition and the installed capture host call
// this same implementation. All owned GL objects require a live owning context.
class StaticDrawPass {
public:
    explicit StaticDrawPass(const std::filesystem::path& resource_root);
    ~StaticDrawPass();
    StaticDrawPass(const StaticDrawPass&) = delete;
    StaticDrawPass& operator=(const StaticDrawPass&) = delete;
    StaticDrawStats Render(const RenderView& view,
                           std::shared_ptr<const StaticDrawSnapshot> snapshot,
                           const GBuffer& target,
                           bool temporal_enabled = false);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Luminumbra::Rendering
