#pragma once
#include "DiagnosticCapture.h"
#include "DiagnosticTargets.h"
#include "GBuffer.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace Luminumbra::Rendering {
class RenderResourceRegistry;
class DiagnosticDrawTable;
struct DiagnosticSubmission;
class Shader;

// Render-thread attribution scope; keep the creating GL context current until
// destruction. The table and optional target owner must outlive this scope.
// GBufferPass leases storage during deferred submission, including on exceptions.
// Completion requires detachment, lease release and complete deferred coverage.
// Later contributors need their own coverage before bundle publication.
// Construction performs no GL work; table-only scopes cannot attach or complete.
class GBufferDiagnosticCapture {
public:
    explicit GBufferDiagnosticCapture(DiagnosticDrawTable& table);
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
    // Detached storage can be displayed with unsupported records hatched. This
    // does not certify identity coverage; publication must still use complete().
    bool snapshot_ready() const;
    // Const diagnostic: sticky capture failure, including retirement and unwinding.
    bool attachment_failed() const noexcept;
    // Sticky submission refusal, separate from unsupported producer coverage.
    bool refused() const;
    bool deferred_attached() const;
    // The caller keeps the forward framebuffer alive until detach (or scope exit).
    bool attach_forward(std::uint32_t framebuffer,
                        std::uint32_t color_texture,
                        const DiagnosticExtent& extent);
    Shader*
    variant(Shader* shader, bool ladder, const std::vector<std::string>& feedback_varyings = {});
    void write(const Shader& shader, const DiagnosticSubmission& submission);
    void write_current(const DiagnosticSubmission& submission);
    // Refused/uncovered fallback draws must not inherit the preceding identity.
    // No-op unless a variant owned by this capture is currently bound.
    void clear_current_identity();
    void unsupported(std::string_view producer,
                     std::uint32_t lod = 0,
                     DiagnosticOwnership ownership = DiagnosticOwnership::Live,
                     DiagnosticPass pass = DiagnosticPass::GBuffer);

private:
    struct State {
        explicit State(DiagnosticDrawTable& draw_table);
        ~State();
        DiagnosticTargets* targets = nullptr;
        DiagnosticDrawTable& table;
        // The target owner outlives this scope. Detach before resetting the lease.
        DiagnosticTargets::AttachmentLease attachment_lease;
        DiagnosticTargets* forward_targets = nullptr;
        std::uint32_t framebuffer = 0;
        // GL guarantees at least eight draw buffers; refuse contexts above this
        // bounded snapshot capacity rather than partially restoring their state.
        std::array<std::uint32_t, 32> buffers{};
        std::array<std::uint8_t, 4> color_mask{};
        int buffer_count = 0;
        bool failed = false;
        bool snapshot_failed = false;
        bool captured = false;
        bool refused = false;
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
        // Deferred and contributor variants share the six-program capture budget.
        std::vector<Variant> variants;
        std::string_view failure;
    };
    static void SetCurrent(GBufferDiagnosticCapture* capture);
    bool
    attach_target(const GBuffer& gbuffer, const DiagnosticExtent& extent, bool forward = false);
    void detach_target();
    std::unique_ptr<State> state_;
};

// Optional explicit attachment scope, nested inside its active capture's lifetime.
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
