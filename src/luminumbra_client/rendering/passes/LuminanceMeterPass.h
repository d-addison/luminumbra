#pragma once

#include "../AsyncReadbackRing.h"
#include "../RenderContext.h"

#include <filesystem>
#include <glad/glad.h>

namespace Luminumbra::Rendering {

// Shared compute-program helper used by the luminance meter and froxel passes.
GLuint create_compute_program(const char* compute_source);

// GPU auto-exposure metering pass extracted from RenderPipeline. Its compute
// program, reduction SSBO, and asynchronous readback ring are pass-owned; the
// resolved scene texture arrives through RenderContext.
class LuminanceMeterPass {
public:
    LuminanceMeterPass();
    ~LuminanceMeterPass();

    // Returns false only when lazy initialization fails and metering must be
    // disabled to preserve the existing fallback behavior.
    bool execute(const RenderContext& ctx, const std::filesystem::path& root_path);
    bool initialized() const {
        return m_readback.initialized();
    }
    bool consume(const void** out_ptr, std::size_t* out_bytes) {
        return m_readback.consume(out_ptr, out_bytes);
    }
    void destroy();

private:
    u32 m_reduce_program = 0;
    u32 m_reduce_ssbo = 0;
    AsyncReadbackRing m_readback;
};

} // namespace Luminumbra::Rendering
