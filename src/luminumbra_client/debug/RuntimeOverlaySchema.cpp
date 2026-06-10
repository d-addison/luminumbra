#include "debug/RuntimeOverlaySchema.h"

namespace Luminumbra::Client::Debug {

RuntimeOverlaySchema BuildRuntimeOverlaySchema() {
    return RuntimeOverlaySchema{
        {
            "total_chunks",
            "ready_chunks",
            "loading_chunks",
            "meshing_chunks",
            "renderable_chunks",
            "collision_chunks",
            "lod_distribution",
            "seam_risk_samples",
        },
        {
            "generation_job_active",
            "meshing_job_active",
            "generation_budget",
            "meshing_budget",
            "deferred_generation",
            "deferred_meshing",
            "upload_backlog_high_water",
        },
        {
            "pass_name",
            "draw_count",
            "shadow_draws",
            "terrain_draws",
            "water_draws",
            "skybox_draws",
            "estimated_vram_bytes",
            "resource_registry",
        },
        {
            "frame_time_ms_p50",
            "frame_time_ms_p95",
            "frame_time_ms_p99",
            "memory_high_water",
            "job_queue_high_water",
            "regression_budget_passed",
        },
        {
            "geometry_shader_ok",
            "lighting_shader_ok",
            "terrain_material_fallback_layers",
            "shader_diagnostics",
        },
        {
            "not_requested",
            "density_pending",
            "meshing_failed",
            "upload_pending",
            "culled",
            "shader_failed",
            "resource_missing",
        },
    };
}

std::string RuntimeOverlaySchemaName() {
    return "luminumbra.tooling.runtime_overlay.v1";
}

} // namespace Luminumbra::Client::Debug
