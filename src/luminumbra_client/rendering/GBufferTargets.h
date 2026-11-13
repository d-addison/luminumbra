#pragma once

#include "DiagnosticCapture.h"
#include "FarWorldSelection.h"
#include "GBuffer.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace Luminumbra::Rendering {
class RenderResourceRegistry;
class DiagnosticTargets;
class DiagnosticDrawTable;
struct DiagnosticSubmission;
class Shader;

// Capture adapter seam. One render-thread scope, with its creating GL context
// current until destruction. The adapter owns targets/table and serializes only
// when complete() is true. Construction alone performs no GL work.
// Completion covers GBufferPass only; later deferred and forward contributors
// require their own coverage before an adapter can publish a complete bundle.
class GBufferDiagnosticCapture {
public:
    // An optional distinct target records forward foliage contributors, never
    // overwriting deferred identities. Both targets and the table outlive the scope.
    GBufferDiagnosticCapture(DiagnosticTargets& targets,
                             DiagnosticDrawTable& table,
                             DiagnosticTargets* forward_targets = nullptr);
    ~GBufferDiagnosticCapture();
    GBufferDiagnosticCapture(const GBufferDiagnosticCapture&) = delete;
    GBufferDiagnosticCapture& operator=(const GBufferDiagnosticCapture&) = delete;
    static GBufferDiagnosticCapture* Current();
    bool attach(const GBuffer& gbuffer, const DiagnosticExtent& extent);
    void detach();
    void before_destroy(const GBuffer& gbuffer);
    // An aborted frame is never eligible for publication, even if its table is empty.
    void abort() noexcept;
    bool complete() const;
    bool deferred_attached() const;
    bool attach_forward(std::uint32_t framebuffer,
                        std::uint32_t color_texture,
                        const DiagnosticExtent& extent);
    Shader*
    variant(Shader* shader, bool ladder, const std::vector<std::string>& feedback_varyings = {});
    void write(const Shader& shader, const DiagnosticSubmission& submission);
    void write_current(const DiagnosticSubmission& submission);
    void prepare_ladder(const FarWorldDrawCommand& command);
    bool write_ladder(std::int32_t index_count) noexcept;
    void unsupported(std::string_view producer,
                     std::uint32_t lod = 0,
                     DiagnosticOwnership ownership = DiagnosticOwnership::Live,
                     DiagnosticPass pass = DiagnosticPass::GBuffer);

private:
    struct State {
        State(DiagnosticTargets& target_storage, DiagnosticDrawTable& draw_table);
        ~State();
        DiagnosticTargets& targets;
        DiagnosticDrawTable& table;
        DiagnosticTargets* forward_targets = nullptr;
        std::uint32_t framebuffer = 0;
        // GL guarantees at least eight draw buffers; refuse contexts above this
        // bounded snapshot capacity rather than partially restoring their state.
        std::array<std::uint32_t, 32> buffers{};
        std::array<std::uint8_t, 4> color_mask{};
        int buffer_count = 0;
        bool failed = false;
        bool captured = false;
        bool forward = false;
        bool forward_captured = false;
        struct Variant {
            std::string vertex;
            std::string fragment;
            bool ladder;
            std::vector<std::string> feedback_varyings;
            std::unique_ptr<Shader> shader;
            int identity;
        };
        // Three selected deferred variants, authored surfaces, impostors and foliage.
        std::vector<Variant> variants;
        FarWorldDrawCommand ladder_command{};
        bool ladder_prepared = false;
        std::string_view failure;
    };
    static void SetCurrent(GBufferDiagnosticCapture* capture);
    bool
    attach_target(const GBuffer& gbuffer, const DiagnosticExtent& extent, bool forward = false);
    void detach_target();
    std::unique_ptr<State> state_;
};

class GBufferDiagnosticAttachment {
public:
    GBufferDiagnosticAttachment(const GBuffer& gbuffer, const DiagnosticExtent& extent);
    GBufferDiagnosticAttachment(std::uint32_t framebuffer,
                                std::uint32_t color_texture,
                                const DiagnosticExtent& extent);
    ~GBufferDiagnosticAttachment();
    GBufferDiagnosticAttachment(const GBufferDiagnosticAttachment&) = delete;
    GBufferDiagnosticAttachment& operator=(const GBufferDiagnosticAttachment&) = delete;

private:
    GBufferDiagnosticCapture* capture_;
    int exceptions_ = 0;
};
void CreateGBufferTargets(GBuffer& gbuffer,
                          RenderResourceRegistry& registry,
                          u32 width,
                          u32 height,
                          bool authored = false);
void DestroyGBufferTargets(GBuffer& gbuffer, RenderResourceRegistry& registry);
} // namespace Luminumbra::Rendering
