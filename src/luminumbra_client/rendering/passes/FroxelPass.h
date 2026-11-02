#pragma once

#include "../AttributedGl.h"
#include "../RenderContext.h"

#include <filesystem>

namespace Luminumbra::Rendering {

// Froxel volumetric pass extracted from RenderPipeline. Injection and
// integration stay together because they share the compute programs and 3D
// scatter/integrated volumes.
//
// Converted to the RenderContext seam: injection reads volumetric quality,
// camera, output extent, sun/ambient lighting, shadow depth/tint arrays,
// cascade splits, and light-space matrices from ctx; integration reads quality.
// The shader root is pass-specific configuration. Both 3D textures and both
// programs remain pass-owned raw GL objects with the same lazy-init lifetime.
class FroxelPass {
public:
    FroxelPass();
    ~FroxelPass();

    // Initialization failure disables volumetrics until destroy() explicitly
    // releases the state and permits a retry. Frozen replay never initializes.
    bool execute_inject(const RenderContext& ctx, const std::filesystem::path& root_path);
    void execute_integrate(const RenderContext& ctx);
    void destroy(); // Also clears a latched initialization failure for retry.

    u32 integrated_texture() const {
        return m_integrated_tex;
    }

private:
    enum class InitializationState {
        Uninitialized,
        Ready,
        Failed
    };
    InitializationState m_initialization_state = InitializationState::Uninitialized;
    AttributedGl::Program m_inject_program{};
    AttributedGl::Program m_integrate_program{};
    u32 m_scatter_tex = 0;    // rgba16f 160x90x64: rgb in-scatter, a sigma
    u32 m_integrated_tex = 0; // rgba16f 160x90x64: rgb accumulated L, a T
};

} // namespace Luminumbra::Rendering
