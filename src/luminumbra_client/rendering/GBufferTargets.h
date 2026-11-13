#pragma once
#include "DiagnosticCapture.h"
#include "GBuffer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace Luminumbra::Rendering {
class RenderResourceRegistry;
class DiagnosticDrawTable;
struct DiagnosticSubmission;
class Shader;

// Render-thread attribution producer scope. Keep the creating GL context current
// until destruction. Construction performs no GL work; no runtime option creates
// this scope yet. It never owns or exposes a target texture or attaches one to a
// framebuffer. complete() remains false until attachment integration is supplied.
class GBufferDiagnosticCapture {
public:
    explicit GBufferDiagnosticCapture(DiagnosticDrawTable& table);
    ~GBufferDiagnosticCapture();
    GBufferDiagnosticCapture(const GBufferDiagnosticCapture&) = delete;
    GBufferDiagnosticCapture& operator=(const GBufferDiagnosticCapture&) = delete;
    static GBufferDiagnosticCapture* Current();
    bool complete() const;
    Shader* variant(Shader* shader, bool ladder);
    void write(const Shader& shader, const DiagnosticSubmission& submission);
    void unsupported(std::string_view producer,
                     std::uint32_t lod = 0,
                     DiagnosticOwnership ownership = DiagnosticOwnership::Live);

private:
    struct State {
        explicit State(DiagnosticDrawTable& draw_table);
        ~State();
        DiagnosticDrawTable& table;
        struct Variant {
            std::string vertex;
            std::string fragment;
            bool ladder;
            std::unique_ptr<Shader> shader;
            int identity;
        };
        // Three deferred vertex stages, each with default and ladder variants.
        std::vector<Variant> variants;
    };
    static void SetCurrent(GBufferDiagnosticCapture* capture);
    std::unique_ptr<State> state_;
};

void CreateGBufferTargets(GBuffer& gbuffer,
                          RenderResourceRegistry& registry,
                          u32 width,
                          u32 height,
                          bool authored = false);
void DestroyGBufferTargets(GBuffer& gbuffer, RenderResourceRegistry& registry);
} // namespace Luminumbra::Rendering
