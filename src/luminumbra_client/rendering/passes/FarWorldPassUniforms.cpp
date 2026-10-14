#include "FarWorldPassUniforms.h"

#include "../FarWorldOwnership.h"
#include "../Shader.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <cmath>

namespace Luminumbra::Rendering {
void FarWorldPassUniforms::Cache(GLuint program) noexcept {
    shader_ = nullptr;
    CacheLocations(program);
    Reset();
}
void FarWorldPassUniforms::Cache(const Shader& shader) noexcept {
    shader_ = &shader;
    generation_ = shader.ProgramGeneration();
    CacheLocations(shader.IsValid() ? shader.Id() : 0);
    Reset();
}
void FarWorldPassUniforms::Refresh() noexcept {
    if (shader_ && generation_ != shader_->ProgramGeneration()) {
        generation_ = shader_->ProgramGeneration();
        CacheLocations(shader_->IsValid() ? shader_->Id() : 0);
        first_ = true;
        tier_edge_ = 0;
    }
}
void FarWorldPassUniforms::CacheLocations(GLuint program) noexcept {
    program_ = program;
    if (!program_)
        return;
    ladder_ = glGetUniformLocation(program_, "u_farWorldLadder");
    bias_ = glGetUniformLocation(program_, "u_farRayBiasMeters");
    near_ = glGetUniformLocation(program_, "u_farClipNearRadius");
    far_ = glGetUniformLocation(program_, "u_farClipFarRadius");
    instance_ = glGetUniformLocation(program_, "u_useInstanceOrigin");
    outer_ = glGetUniformLocation(program_, "u_farOuterRadius");
    edge_ = glGetUniformLocation(program_, "u_farTileEdge");
    arrival_ = glGetUniformLocation(program_, "u_farArrivalEligible");
    finer_outer_ = glGetUniformLocation(program_, "u_farFinerOuterRadius");
    children_ = glGetUniformLocation(program_, "u_farChildY[0]");
    chunk_ = glGetUniformLocation(program_, "u_farOriginChunk");
    remainder_ = glGetUniformLocation(program_, "u_farOriginRemainder");
    count_ = glGetUniformLocation(program_, "u_farLiveCellCount");
    relative_ = glGetUniformLocation(program_, "u_farRelativeOrigin");
    world_ = glGetUniformLocation(program_, "u_farWorldOrigin");
}
void FarWorldPassUniforms::Reset() noexcept {
    Refresh();
    if (!program_)
        return;
    glProgramUniform1i(program_, ladder_, 0);
    glProgramUniform1f(program_, bias_, 0);
    glProgramUniform1i(program_, count_, 0);
    glProgramUniform1i(program_, arrival_, 0);
    glProgramUniform1f(program_, near_, 0);
    glProgramUniform1f(program_, far_, 0);
}
void FarWorldPassUniforms::UseInstanceOrigin(bool enabled) noexcept {
    Refresh();
    if (program_)
        glProgramUniform1i(program_, instance_, enabled ? 1 : 0);
}
void FarWorldPassUniforms::Begin() noexcept {
    // One origin-mode transition at the boundary after live MDI, for either
    // selected far renderer. Ladder never inherits the live instance attribute.
    UseInstanceOrigin(false);
    first_ = true;
    tier_edge_ = 0;
}
bool FarWorldPassUniforms::Prepare(const FarWorldDrawCommand& command) noexcept {
    Refresh();
    if (!program_ || !std::isfinite(command.view_ray_bias_meters) ||
        command.view_ray_bias_meters < 0 || command.live_cell_count < 0 ||
        command.live_cell_count > static_cast<std::int32_t>(kFarWorldLiveCellLimit))
        return false;
    if (first_) {
        glProgramUniform1i(program_, ladder_, 1);
        glProgramUniform1f(program_, near_, static_cast<float>(kFarWorldInnerRadiusMeters));
        glProgramUniform1f(program_, far_, 0);
        glProgramUniform1i(program_, count_, command.live_cell_count);
        first_ = false;
    }
    // Residency visits tier-contiguous V5 -> V1. Spacing, edge and radii are
    // tier invariants; every tile still uploads its exact origin/arrival spans.
    if (tier_edge_ != command.tile_edge_meters) {
        tier_edge_ = command.tile_edge_meters;
        glProgramUniform1f(program_, edge_, tier_edge_);
        glProgramUniform1f(
            program_, outer_, static_cast<float>(command.horizontal_outer_clip_meters));
        // Ineligible arrival records leave their finer radius at zero. Derive
        // this tier invariant from the table so a later eligible tile in the
        // same tier receives the correct clip-aware replacement radius.
        float finer_radius = 0;
        for (std::size_t tier = 2; tier <= World::kFarTierCount; ++tier) {
            if (tier_edge_ == static_cast<float>(World::kFarTierTable[tier - 1].tile_edge_meters)) {
                finer_radius = static_cast<float>(World::FarWantedRadiusMeters(tier - 1));
                break;
            }
        }
        glProgramUniform1f(program_, finer_outer_, finer_radius);
        glProgramUniform1f(program_, bias_, static_cast<float>(command.view_ray_bias_meters));
    }
    glProgramUniform1i(program_, arrival_, command.arrival_eligible ? 1 : 0);
    // Flatten without relying on padding between nested std::array objects.
    GLfloat spans[8]{};
    for (std::size_t i = 0; i < command.child_local_y_spans.size(); ++i) {
        spans[2 * i] = command.child_local_y_spans[i][0];
        spans[2 * i + 1] = command.child_local_y_spans[i][1];
    }
    glProgramUniform2fv(program_, children_, 4, spans);
    glProgramUniform3iv(program_, chunk_, 1, command.origin_chunk.data());
    glProgramUniform3fv(program_, remainder_, 1, command.origin_remainder.data());
    glProgramUniform3fv(program_, relative_, 1, command.relative_origin.data());
    glProgramUniform3fv(program_, world_, 1, command.world_origin.data());
    return true;
}
} // namespace Luminumbra::Rendering
