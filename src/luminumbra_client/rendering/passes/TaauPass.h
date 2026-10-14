#pragma once

#include "../FarWorldReplay.h"
#include "../RenderContext.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class RenderResourceRegistry;
class Shader;

// TAAU resolve pass extracted from RenderPipeline. Owns the resolve shader and
// transient FBO; the two history textures remain registry-owned.
class TaauPass {
public:
    TaauPass();
    ~TaauPass();

    void init_shader(const std::filesystem::path& root_path);
    void init(RenderResourceRegistry& registry, u32 width, u32 height);
    void destroy(RenderResourceRegistry& registry);
    void reset_shader();
    void invalidate_history();
    void execute(const RenderContext& ctx, const TaauHistoryState* frozen = nullptr);
    TaauHistoryState history_state() const noexcept {
        return {{m_history[0], m_history[1]}, m_history_write, m_history_valid};
    }
    bool replay_available() const noexcept {
        return m_replay_output != 0 && m_replay_fbo != 0;
    }

private:
    std::unique_ptr<Shader> m_shader;
    u32 m_fbo = 0;
    u32 m_replay_fbo = 0;
    u32 m_replay_output = 0;     // Scratch resolve; never aliases either history texture.
    u32 m_history[2] = {0u, 0u}; // RGBA16F resolved-color history (ping-pong)
    int m_history_write = 0;
    bool m_history_valid = false;
};

} // namespace Luminumbra::Rendering
